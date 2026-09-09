#include "qemuReadySetExecutor.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace SST {
namespace Mittens {

QemuReadySetExecutionError::QemuReadySetExecutionError(
    std::uint32_t tileId,
    const std::string& message) :
    std::runtime_error(
        "ready-set QEMU tile " + std::to_string(tileId) + ": " +
        message),
    tileId_(tileId)
{}

QemuReadySetExecutor::QemuReadySetExecutor(std::size_t workerCount)
{
    if (workerCount == 0) {
        throw std::invalid_argument(
            "ready-set QEMU executor requires at least one worker");
    }

    workers_.reserve(workerCount);
    try {
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    } catch (...) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

QemuReadySetExecutor::~QemuReadySetExecutor()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    workAvailable_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void QemuReadySetExecutor::begin(
    std::uint64_t frontierTick,
    std::size_t expectedTasks)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        throw std::logic_error("ready-set QEMU executor is stopping");
    }
    if (batchActive_) {
        throw std::logic_error("ready-set QEMU batch is already active");
    }
    if (expectedTasks == 0) {
        throw std::invalid_argument(
            "ready-set QEMU batch cannot be empty");
    }
    if (!queue_.empty() || !batch_.empty() || finishedTasks_ != 0) {
        throw std::logic_error(
            "ready-set QEMU executor retained stale batch state");
    }

    frontierTick_ = frontierTick;
    expectedTasks_ = expectedTasks;
    ownerThread_ = std::this_thread::get_id();
    batchActive_ = true;
}

void QemuReadySetExecutor::submit(Task task)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!batchActive_) {
        throw std::logic_error("ready-set QEMU batch is not active");
    }
    if (ownerThread_ != std::this_thread::get_id()) {
        throw std::logic_error(
            "ready-set QEMU task was submitted from a non-owner thread");
    }
    if (task.frontierTick != frontierTick_) {
        throw QemuReadySetExecutionError(
            task.tileId,
            "task frontier " + std::to_string(task.frontierTick) +
                " does not match batch frontier " +
                std::to_string(frontierTick_));
    }
    if (batch_.size() >= expectedTasks_) {
        throw QemuReadySetExecutionError(
            task.tileId, "batch received more tasks than declared");
    }
    if (task.grantEpoch == 0) {
        throw QemuReadySetExecutionError(
            task.tileId, "grant epoch must be nonzero");
    }
    if (!task.capture || !task.validate ||
        !task.modeledDeliveryTick || !task.commit) {
        throw QemuReadySetExecutionError(
            task.tileId, "task is missing a required callback");
    }
    if (!submittedTiles_.insert(task.tileId).second) {
        throw QemuReadySetExecutionError(
            task.tileId, "tile was submitted more than once");
    }

    const std::uint64_t submissionSequence = batch_.size();
    auto item = std::make_shared<WorkItem>(
        std::move(task), submissionSequence);
    batch_.push_back(item);
    queue_.push_back(std::move(item));
    workAvailable_.notify_one();
}

std::vector<QemuReadySetExecutor::Completion>
QemuReadySetExecutor::collect()
{
    std::vector<std::shared_ptr<WorkItem>> batch;
    std::uint64_t frontier = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!batchActive_) {
            throw std::logic_error("ready-set QEMU batch is not active");
        }
        if (ownerThread_ != std::this_thread::get_id()) {
            throw std::logic_error(
                "ready-set QEMU batch was collected from a non-owner thread");
        }
        if (batch_.size() != expectedTasks_) {
            throw std::logic_error(
                "ready-set QEMU batch is incomplete: submitted " +
                std::to_string(batch_.size()) + " of " +
                std::to_string(expectedTasks_));
        }
        batchFinished_.wait(lock, [this]() {
            return finishedTasks_ == expectedTasks_;
        });
        batch = batch_;
        frontier = frontierTick_;
        clearBatchLocked();
    }

    auto byTile = [](const std::shared_ptr<WorkItem>& left,
                     const std::shared_ptr<WorkItem>& right) {
        return left->task.tileId < right->task.tileId;
    };
    std::sort(batch.begin(), batch.end(), byTile);

    for (const auto& item : batch) {
        if (item->captureError) {
            throw QemuReadySetExecutionError(
                item->task.tileId,
                "capture failed: " +
                    describeException(item->captureError));
        }
        if (!item->event.has_value()) {
            throw QemuReadySetExecutionError(
                item->task.tileId,
                "capture completed without an event");
        }
    }

    std::vector<Completion> completions;
    completions.reserve(batch.size());
    std::map<std::uint32_t, std::size_t> capturedStopReasons;
    std::size_t capturedBatchedEvents = 0;
    for (const auto& item : batch) {
        const QemuSyncEvent& event = *item->event;
        ++capturedStopReasons[event.stopReason];
        if (!event.memoryBatch.empty() ||
            (event.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0) {
            ++capturedBatchedEvents;
        }
    }
    for (const auto& item : batch) {
        const QemuSyncEvent& event = *item->event;
        if (event.grantEpoch != item->task.grantEpoch) {
            throw QemuReadySetExecutionError(
                item->task.tileId,
                "event grant epoch " +
                    std::to_string(event.grantEpoch) +
                    " does not match expected epoch " +
                    std::to_string(item->task.grantEpoch));
        }
        if (event.eventSequence == 0) {
            throw QemuReadySetExecutionError(
                item->task.tileId,
                "event sequence must be nonzero");
        }

        std::uint64_t delivery = 0;
        try {
            item->task.validate(event);
            delivery = item->task.modeledDeliveryTick(event);
        } catch (...) {
            std::ostringstream message;
            message << "validation failed: "
                    << describeException(std::current_exception())
                    << "; captured_stop_reasons=";
            bool first = true;
            for (const auto& [reason, count] : capturedStopReasons) {
                if (!first) {
                    message << ',';
                }
                first = false;
                message << reason << ':' << count;
            }
            message << "; captured_batched_events="
                    << capturedBatchedEvents;
            throw QemuReadySetExecutionError(
                item->task.tileId, message.str());
        }
        if (delivery < frontier) {
            throw QemuReadySetExecutionError(
                item->task.tileId,
                "modeled delivery tick precedes the batch frontier");
        }

        completions.push_back({
            delivery,
            item->submissionSequence,
            item->task.tileId,
            item->task.grantEpoch,
            event.eventSequence,
            event,
            item->task.commit,
        });
    }

    std::sort(
        completions.begin(),
        completions.end(),
        [](const Completion& left, const Completion& right) {
            return std::tie(
                       left.modeledDeliveryTick,
                       left.submissionSequence,
                       left.tileId,
                       left.grantEpoch,
                       left.eventSequence) <
                   std::tie(
                       right.modeledDeliveryTick,
                       right.submissionSequence,
                       right.tileId,
                       right.grantEpoch,
                       right.eventSequence);
        });
    return completions;
}

void QemuReadySetExecutor::discard()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!batchActive_) {
        return;
    }
    if (ownerThread_ != std::this_thread::get_id()) {
        throw std::logic_error(
            "ready-set QEMU batch was discarded from a non-owner thread");
    }
    const std::size_t submitted = batch_.size();
    batchFinished_.wait(lock, [this, submitted]() {
        return finishedTasks_ == submitted;
    });
    clearBatchLocked();
}

std::size_t QemuReadySetExecutor::submittedTaskCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return batch_.size();
}

bool QemuReadySetExecutor::active() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return batchActive_;
}

std::string QemuReadySetExecutor::describeException(
    const std::exception_ptr& error)
{
    if (!error) {
        return "unknown error";
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "non-standard exception";
    }
}

void QemuReadySetExecutor::workerLoop()
{
    while (true) {
        std::shared_ptr<WorkItem> item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this]() {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) {
                return;
            }
            item = queue_.front();
            queue_.pop_front();
        }

        try {
            item->event = item->task.capture();
        } catch (...) {
            item->captureError = std::current_exception();
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++finishedTasks_;
        }
        batchFinished_.notify_one();
    }
}

void QemuReadySetExecutor::clearBatchLocked()
{
    queue_.clear();
    batch_.clear();
    submittedTiles_.clear();
    frontierTick_ = 0;
    expectedTasks_ = 0;
    finishedTasks_ = 0;
    ownerThread_ = std::thread::id{};
    batchActive_ = false;
}

QemuAsyncCaptureExecutor::QemuAsyncCaptureExecutor(
    std::size_t workerCount)
{
    if (workerCount == 0) {
        throw std::invalid_argument(
            "asynchronous QEMU capture executor requires at least one worker");
    }
    workers_.reserve(workerCount);
    try {
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    } catch (...) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

QemuAsyncCaptureExecutor::~QemuAsyncCaptureExecutor()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    workAvailable_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::future<QemuSyncEvent> QemuAsyncCaptureExecutor::submit(
    Capture capture)
{
    if (!capture) {
        throw std::invalid_argument(
            "asynchronous QEMU capture callback is empty");
    }
    auto item = std::make_shared<WorkItem>(std::move(capture));
    std::future<QemuSyncEvent> result = item->result.get_future();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::logic_error(
                "asynchronous QEMU capture executor is stopping");
        }
        queue_.push_back(std::move(item));
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }
    workAvailable_.notify_one();
    return result;
}

QemuAsyncCaptureExecutor::Statistics
QemuAsyncCaptureExecutor::statistics() const noexcept
{
    return {
        submitted_.load(std::memory_order_relaxed),
        completed_.load(std::memory_order_relaxed),
        maximumConcurrent_.load(std::memory_order_relaxed),
    };
}

void QemuAsyncCaptureExecutor::updateMaximumConcurrent(
    std::size_t active) noexcept
{
    std::size_t observed =
        maximumConcurrent_.load(std::memory_order_relaxed);
    while (observed < active &&
           !maximumConcurrent_.compare_exchange_weak(
               observed, active,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
}

void QemuAsyncCaptureExecutor::workerLoop()
{
    while (true) {
        std::shared_ptr<WorkItem> item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this]() {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) {
                return;
            }
            item = queue_.front();
            queue_.pop_front();
        }

        const std::size_t active =
            active_.fetch_add(1, std::memory_order_relaxed) + 1;
        updateMaximumConcurrent(active);
        try {
            item->result.set_value(item->capture());
        } catch (...) {
            item->result.set_exception(std::current_exception());
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
        completed_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Mittens
} // namespace SST
