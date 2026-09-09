#ifndef SST_MITTENS_QEMU_READY_SET_EXECUTOR_H
#define SST_MITTENS_QEMU_READY_SET_EXECUTOR_H

#include "../bridge/sharedSyncMemoryBridge.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SST {
namespace Mittens {

class QemuReadySetExecutionError : public std::runtime_error
{
  public:
    QemuReadySetExecutionError(
        std::uint32_t tileId,
        const std::string& message);

    std::uint32_t tileId() const noexcept { return tileId_; }

  private:
    std::uint32_t tileId_;
};

/*
 * Runs the host-only capture portion of independent QEMU grants in parallel.
 * The owner remains responsible for proving task independence and for calling
 * every returned commit callback from its SST event thread.  Workers execute
 * capture callbacks only; validation, modeled-time calculation, ordering, and
 * commit are deliberately kept out of worker threads.
 */
class QemuReadySetExecutor final
{
  public:
    using Capture = std::function<QemuSyncEvent()>;
    using Validate = std::function<void(const QemuSyncEvent&)>;
    using ModeledDeliveryTick =
        std::function<std::uint64_t(const QemuSyncEvent&)>;
    using Commit = std::function<void(const QemuSyncEvent&)>;

    struct Task {
        std::uint64_t frontierTick;
        std::uint32_t tileId;
        std::uint64_t grantEpoch;
        Capture capture;
        Validate validate;
        ModeledDeliveryTick modeledDeliveryTick;
        Commit commit;
    };

    struct Completion {
        std::uint64_t modeledDeliveryTick;
        std::uint64_t submissionSequence;
        std::uint32_t tileId;
        std::uint64_t grantEpoch;
        std::uint64_t eventSequence;
        QemuSyncEvent event;
        Commit commit;
    };

    explicit QemuReadySetExecutor(std::size_t workerCount);
    ~QemuReadySetExecutor();

    QemuReadySetExecutor(const QemuReadySetExecutor&) = delete;
    QemuReadySetExecutor& operator=(
        const QemuReadySetExecutor&) = delete;
    QemuReadySetExecutor(QemuReadySetExecutor&&) = delete;
    QemuReadySetExecutor& operator=(QemuReadySetExecutor&&) = delete;

    void begin(
        std::uint64_t frontierTick,
        std::size_t expectedTasks);
    void submit(Task task);
    std::vector<Completion> collect();
    void discard();

    std::size_t workerCount() const noexcept { return workers_.size(); }
    std::size_t submittedTaskCount() const;
    bool active() const;

  private:
    struct WorkItem {
        WorkItem(Task value, std::uint64_t sequence) :
            task(std::move(value)), submissionSequence(sequence)
        {}

        Task task;
        std::uint64_t submissionSequence;
        std::optional<QemuSyncEvent> event;
        std::exception_ptr captureError;
    };

    static std::string describeException(
        const std::exception_ptr& error);
    void workerLoop();
    void clearBatchLocked();

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable batchFinished_;
    std::deque<std::shared_ptr<WorkItem>> queue_;
    std::vector<std::shared_ptr<WorkItem>> batch_;
    std::unordered_set<std::uint32_t> submittedTiles_;
    std::vector<std::thread> workers_;
    std::uint64_t frontierTick_ = 0;
    std::size_t expectedTasks_ = 0;
    std::size_t finishedTasks_ = 0;
    std::thread::id ownerThread_{};
    bool batchActive_ = false;
    bool stopping_ = false;
};

/*
 * Bounded host executor for causal QEMU lookahead. Unlike a ready set, each
 * capture has an independent modeled commit frontier, so submission returns a
 * future and never asks a worker to validate or commit simulator state.
 */
class QemuAsyncCaptureExecutor final
{
  public:
    using Capture = std::function<QemuSyncEvent()>;

    struct Statistics {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::size_t maximumConcurrent = 0;
    };

    explicit QemuAsyncCaptureExecutor(std::size_t workerCount);
    ~QemuAsyncCaptureExecutor();

    QemuAsyncCaptureExecutor(const QemuAsyncCaptureExecutor&) = delete;
    QemuAsyncCaptureExecutor& operator=(
        const QemuAsyncCaptureExecutor&) = delete;
    QemuAsyncCaptureExecutor(QemuAsyncCaptureExecutor&&) = delete;
    QemuAsyncCaptureExecutor& operator=(
        QemuAsyncCaptureExecutor&&) = delete;

    std::future<QemuSyncEvent> submit(Capture capture);
    Statistics statistics() const noexcept;
    std::size_t workerCount() const noexcept { return workers_.size(); }

  private:
    struct WorkItem {
        explicit WorkItem(Capture value) : capture(std::move(value)) {}

        Capture capture;
        std::promise<QemuSyncEvent> result;
    };

    void workerLoop();
    void updateMaximumConcurrent(std::size_t active) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::deque<std::shared_ptr<WorkItem>> queue_;
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> maximumConcurrent_{0};
    bool stopping_ = false;
};

} // namespace Mittens
} // namespace SST

#endif
