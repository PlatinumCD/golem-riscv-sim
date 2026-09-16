#include "qemuCaptureExecutor.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace SST {
namespace Mittens {

QemuCaptureExecutionError::QemuCaptureExecutionError(
    std::uint32_t tileId,
    const std::string& message) :
    std::runtime_error(
        "QEMU capture tile " + std::to_string(tileId) + ": " +
        message),
    tileId_(tileId)
{}

QemuCaptureExecutor::QemuCaptureExecutor(std::size_t workerCount)
{
    if (workerCount == 0) {
        throw std::invalid_argument(
            "QEMU capture executor requires at least one worker");
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

QemuCaptureExecutor::~QemuCaptureExecutor()
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

void QemuCaptureExecutor::begin(
    std::uint64_t frontierTick,
    std::size_t expectedTasks)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        throw std::logic_error("QEMU capture executor is stopping");
    }
    if (batchActive_) {
        throw std::logic_error("QEMU capture batch is already active");
    }
    if (expectedTasks == 0) {
        throw std::invalid_argument(
            "QEMU capture batch cannot be empty");
    }
    if (!queue_.empty() || !batch_.empty() || finishedTasks_ != 0) {
        throw std::logic_error(
            "QEMU capture executor retained stale batch state");
    }

    frontierTick_ = frontierTick;
    expectedTasks_ = expectedTasks;
    ownerThread_ = std::this_thread::get_id();
    batchActive_ = true;
}

void QemuCaptureExecutor::validateTaskLocked(const Task& task) const
{
    if (stopping_) {
        throw std::logic_error("QEMU capture executor is stopping");
    }
    if (!batchActive_) {
        throw std::logic_error("QEMU capture batch is not active");
    }
    if (ownerThread_ != std::this_thread::get_id()) {
        throw std::logic_error(
            "QEMU capture task was submitted from a non-owner thread");
    }
    if (task.frontierTick != frontierTick_) {
        throw QemuCaptureExecutionError(
            task.tileId,
            "task frontier " + std::to_string(task.frontierTick) +
                " does not match batch frontier " +
                std::to_string(frontierTick_));
    }
    if (batch_.size() >= expectedTasks_) {
        throw QemuCaptureExecutionError(
            task.tileId, "batch received more tasks than declared");
    }
    if (task.grantEpoch == 0) {
        throw QemuCaptureExecutionError(
            task.tileId, "grant epoch must be nonzero");
    }
    if (!task.capture || !task.validate ||
        !task.modeledDeliveryTick || !task.commit) {
        throw QemuCaptureExecutionError(
            task.tileId, "task is missing a required callback");
    }
}

void QemuCaptureExecutor::submit(Task task)
{
    std::unique_lock<std::mutex> lock(mutex_);
    validateTaskLocked(task);
    if (!submittedTiles_.insert(task.tileId).second) {
        throw QemuCaptureExecutionError(
            task.tileId, "tile was submitted more than once");
    }

    const std::uint64_t submissionSequence = batch_.size();
    const auto tileId = task.tileId;
    try {
        auto item = std::make_shared<WorkItem>(
            std::move(task), submissionSequence);
        batch_.push_back(item);
        queue_.push_back(std::move(item));
    } catch (...) {
        if (batch_.size() > submissionSequence)
            batch_.pop_back();
        submittedTiles_.erase(tileId);
        throw;
    }
    lock.unlock();
    workAvailable_.notify_one();
}

void QemuCaptureExecutor::submitBatch(std::vector<Task> tasks)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !batchActive_ || ownerThread_ != std::this_thread::get_id())
            throw std::logic_error("QEMU capture batch submission has no active owner");
        if (!batch_.empty() || !queue_.empty() || finishedTasks_ != 0 ||
            !submittedTiles_.empty() || tasks.size() != expectedTasks_)
            throw std::logic_error("QEMU capture batch submission requires the full empty session");

        // Stage every allocation and validation before publishing anything.
        // Workers cannot observe a prefix, even if a callback or allocation fails.
        std::unordered_set<std::uint32_t> tiles;
        tiles.reserve(tasks.size());
        for (const auto& task : tasks) {
            validateTaskLocked(task);
            if (!tiles.insert(task.tileId).second)
                throw QemuCaptureExecutionError(task.tileId, "tile was submitted more than once");
        }
        std::vector<std::shared_ptr<WorkItem>> batch;
        std::deque<std::shared_ptr<WorkItem>> queue;
        batch.reserve(tasks.size());
        for (auto& task : tasks) {
            auto item = std::make_shared<WorkItem>(std::move(task), batch.size());
            batch.push_back(item);
            queue.push_back(std::move(item));
        }
        batch_.swap(batch);
        queue_.swap(queue);
        submittedTiles_.swap(tiles);
    }
    workAvailable_.notify_all();
}

std::vector<QemuCaptureExecutor::Completion>
QemuCaptureExecutor::collect()
{
    std::vector<std::shared_ptr<WorkItem>> batch;
    std::uint64_t frontier = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!batchActive_) {
            throw std::logic_error("QEMU capture batch is not active");
        }
        if (ownerThread_ != std::this_thread::get_id()) {
            throw std::logic_error(
                "QEMU capture batch was collected from a non-owner thread");
        }
        if (batch_.size() != expectedTasks_) {
            throw std::logic_error(
                "QEMU capture batch is incomplete: submitted " +
                std::to_string(batch_.size()) + " of " +
                std::to_string(expectedTasks_));
        }
        batchFinished_.wait(lock, [this]() {
            return finishedTasks_ == expectedTasks_;
        });
        batch.swap(batch_);
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
            throw QemuCaptureExecutionError(
                item->task.tileId,
                "capture failed: " +
                    describeException(item->captureError));
        }
        if (!item->event.has_value()) {
            throw QemuCaptureExecutionError(
                item->task.tileId,
                "capture completed without an event");
        }
    }

    std::vector<Completion> completions;
    completions.reserve(batch.size());
    for (const auto& item : batch) {
        const QemuSyncEvent& event = *item->event;
        if (event.grantEpoch != item->task.grantEpoch) {
            throw QemuCaptureExecutionError(
                item->task.tileId,
                "event grant epoch " +
                    std::to_string(event.grantEpoch) +
                    " does not match expected epoch " +
                    std::to_string(item->task.grantEpoch));
        }
        if (event.eventSequence == 0) {
            throw QemuCaptureExecutionError(
                item->task.tileId,
                "event sequence must be nonzero");
        }

        std::uint64_t delivery = 0;
        try {
            item->task.validate(event);
            delivery = item->task.modeledDeliveryTick(event);
        } catch (...) {
            std::map<std::uint32_t, std::size_t> capturedStopReasons;
            std::size_t capturedBatchedEvents = 0;
            for (const auto& captured : batch) {
                const auto& value = *captured->event;
                ++capturedStopReasons[value.stopReason];
                if (!value.memoryBatch.empty() ||
                    (value.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0)
                    ++capturedBatchedEvents;
            }
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
            throw QemuCaptureExecutionError(
                item->task.tileId, message.str());
        }
        if (delivery < frontier) {
            throw QemuCaptureExecutionError(
                item->task.tileId,
                "modeled delivery tick precedes the batch frontier");
        }

        completions.push_back({
            delivery,
            item->submissionSequence,
            item->task.tileId,
            item->task.grantEpoch,
            event.eventSequence,
            {},
            {},
        });
    }

    // All validation and error diagnostics above see intact payloads. Only
    // after the entire batch succeeds do completions take their ownership.
    for (std::size_t index = 0; index < batch.size(); ++index) {
        completions[index].event = std::move(*batch[index]->event);
        completions[index].commit = std::move(batch[index]->task.commit);
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

void QemuCaptureExecutor::discard()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!batchActive_) {
        return;
    }
    if (ownerThread_ != std::this_thread::get_id()) {
        throw std::logic_error(
            "QEMU capture batch was discarded from a non-owner thread");
    }
    const std::size_t submitted = batch_.size();
    batchFinished_.wait(lock, [this, submitted]() {
        return finishedTasks_ == submitted;
    });
    clearBatchLocked();
}

std::size_t QemuCaptureExecutor::submittedTaskCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return batch_.size();
}

bool QemuCaptureExecutor::active() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return batchActive_;
}

std::string QemuCaptureExecutor::describeException(
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

void QemuCaptureExecutor::workerLoop()
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

        bool drained;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++finishedTasks_;
            drained = finishedTasks_ == batch_.size();
        }
        if (drained)
            batchFinished_.notify_one();
    }
}

void QemuCaptureExecutor::clearBatchLocked()
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
