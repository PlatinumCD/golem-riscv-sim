#include "qemuCaptureCoordinator.h"
#include <algorithm>
#include <unordered_map>
#include <sys/wait.h>
#include <cerrno>
#include <system_error>
namespace SST::Mittens
{
struct CaptureWorkerCoordinator
{
    std::mutex mutex;
    std::uint32_t workerCount = 0;
    std::unordered_set<std::uint32_t> registeredTiles;
    std::unordered_set<std::uint32_t> pendingTiles;
    std::vector<QemuCaptureExecutor::Task> pendingTasks;
    std::unique_ptr<QemuCaptureExecutor> executor;
    std::optional<std::thread::id> ownerThread;
    std::optional<std::uint64_t> frontierTick;
    std::uint64_t dispatchCount = 0;
    std::uint64_t capturedTaskCount = 0;
    std::uint64_t parallelDispatchCount = 0;
    std::size_t maximumBatchSize = 0;
    std::uint64_t hostDispatchNanoseconds = 0;
    std::uint64_t hostSubmitNanoseconds = 0;
    std::uint64_t hostCollectNanoseconds = 0;
};

namespace {
class HostElapsed final {
    std::uint64_t& total_;
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  public:
    explicit HostElapsed(std::uint64_t& total) : total_(total) {}
    ~HostElapsed() {
        total_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }
};
}

QemuCaptureHostStatistics& QemuCaptureCoordinator::qemuCaptureHostStatistics()
{
    static QemuCaptureHostStatistics statistics;
    return statistics;
}

void QemuCaptureCoordinator::recordQemuCaptureHostTime(
    const QemuSyncEvent& event, std::chrono::steady_clock::duration duration) noexcept
{
    QemuCaptureHostStatistics& statistics = qemuCaptureHostStatistics();
    if (event.stopReason >= statistics.counts.size())
    {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    const std::uint64_t nanoseconds = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    statistics.counts[event.stopReason].fetch_add(1, std::memory_order_relaxed);
    statistics.nanoseconds[event.stopReason].fetch_add(nanoseconds, std::memory_order_relaxed);
    std::uint64_t maximum =
        statistics.maximumNanoseconds[event.stopReason].load(std::memory_order_relaxed);
    while (maximum < nanoseconds &&
           !statistics.maximumNanoseconds[event.stopReason].compare_exchange_weak(
               maximum, nanoseconds, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
    if (event.stopReason == MITTENS_SYNC_STOP_MEMORY_FENCE)
    {
        const std::size_t payloadClass = (!event.memoryBatch.empty() ? 1U : 0U) |
                                         (!event.globalDMASubmitBatch.empty() ? 2U : 0U) |
                                         (!event.analogSubmitBatch.empty() ? 4U : 0U);
        statistics.fencePayloadCounts[payloadClass].fetch_add(1, std::memory_order_relaxed);
        statistics.fencePayloadNanoseconds[payloadClass].fetch_add(nanoseconds,
                                                                   std::memory_order_relaxed);
    }
}

struct LocalQemuLookaheadRegistry
{
    std::mutex mutex;
    std::uint32_t workerCount = 0;
    std::uint32_t expectedTiles = 0;
    std::string independenceProof;
    std::unordered_set<std::uint32_t> registeredTiles;
    std::unique_ptr<QemuAsyncCaptureExecutor> executor;
    std::atomic<std::uint64_t> readyAtCommit{0};
    std::atomic<std::uint64_t> waitedAtCommit{0};
    std::array<std::atomic<std::uint64_t>, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO + 1>
        fusedTerminalCounts{};
};

LocalQemuLookaheadRegistry& localQemuLookaheadRegistry()
{
    static LocalQemuLookaheadRegistry registry;
    return registry;
}

void QemuCaptureCoordinator::registerLocalQemuLookaheadTile(std::uint32_t tileId,
                                                            std::uint32_t workerCount,
                                                            std::uint32_t expectedTiles,
                                                            const std::string& independenceProof)
{
    LocalQemuLookaheadRegistry& registry = localQemuLookaheadRegistry();
    const std::lock_guard<std::mutex> lock(registry.mutex);
    if (registry.registeredTiles.empty())
    {
        registry.workerCount = workerCount;
        registry.expectedTiles = expectedTiles;
        registry.independenceProof = independenceProof;
        registry.executor = std::make_unique<QemuAsyncCaptureExecutor>(workerCount);
    }
    else if (registry.workerCount != workerCount || registry.expectedTiles != expectedTiles ||
             registry.independenceProof != independenceProof)
    {
        throw std::logic_error("local QEMU lookahead configuration differs across tiles");
    }
    if (!registry.registeredTiles.insert(tileId).second ||
        registry.registeredTiles.size() > expectedTiles)
    {
        throw std::logic_error("invalid tile registration in local QEMU lookahead");
    }
}

std::future<QemuSyncEvent>
QemuCaptureCoordinator::submitLocalQemuLookahead(std::uint32_t workerCount,
                                                 QemuAsyncCaptureExecutor::Capture capture)
{
    LocalQemuLookaheadRegistry& registry = localQemuLookaheadRegistry();
    QemuAsyncCaptureExecutor* executor = nullptr;
    {
        const std::lock_guard<std::mutex> lock(registry.mutex);
        if (registry.executor == nullptr || registry.workerCount != workerCount ||
            registry.registeredTiles.size() != registry.expectedTiles)
        {
            throw std::logic_error("local QEMU lookahead is incompletely registered");
        }
        executor = registry.executor.get();
    }
    return executor->submit(std::move(capture));
}

void QemuCaptureCoordinator::recordLocalQemuLookaheadConsume(bool ready) noexcept
{
    LocalQemuLookaheadRegistry& registry = localQemuLookaheadRegistry();
    (ready ? registry.readyAtCommit : registry.waitedAtCommit)
        .fetch_add(1, std::memory_order_relaxed);
}

void QemuCaptureCoordinator::recordLocalQemuLookaheadFusedTerminal(
    std::uint32_t stopReason) noexcept
{
    LocalQemuLookaheadRegistry& registry = localQemuLookaheadRegistry();
    if (stopReason < registry.fusedTerminalCounts.size())
    {
        registry.fusedTerminalCounts[stopReason].fetch_add(1, std::memory_order_relaxed);
    }
}

LocalQemuLookaheadStatistics QemuCaptureCoordinator::localQemuLookaheadStatistics()
{
    LocalQemuLookaheadRegistry& registry = localQemuLookaheadRegistry();
    const std::lock_guard<std::mutex> lock(registry.mutex);
    if (registry.executor == nullptr || registry.registeredTiles.empty())
    {
        throw std::logic_error("local QEMU lookahead statistics are unavailable");
    }
    LocalQemuLookaheadStatistics statistics{
        *std::min_element(registry.registeredTiles.begin(), registry.registeredTiles.end()),
        registry.workerCount,
        registry.executor->statistics(),
        registry.readyAtCommit.load(std::memory_order_relaxed),
        registry.waitedAtCommit.load(std::memory_order_relaxed),
    };
    for (std::size_t reason = 0; reason < statistics.fusedTerminalCounts.size(); ++reason)
    {
        statistics.fusedTerminalCounts[reason] =
            registry.fusedTerminalCounts[reason].load(std::memory_order_relaxed);
    }
    return statistics;
}

CaptureWorkerCoordinator& captureWorkerCoordinator()
{
    static CaptureWorkerCoordinator coordinator;
    return coordinator;
}

CaptureWorkerStatistics QemuCaptureCoordinator::captureWorkerStatistics()
{
    CaptureWorkerCoordinator& coordinator = captureWorkerCoordinator();
    const std::lock_guard<std::mutex> lock(coordinator.mutex);
    if (coordinator.registeredTiles.empty())
    {
        throw std::logic_error("parallel QEMU capture statistics have no registered tiles");
    }
    return {
        *std::min_element(coordinator.registeredTiles.begin(), coordinator.registeredTiles.end()),
        coordinator.dispatchCount,
        coordinator.capturedTaskCount,
        coordinator.parallelDispatchCount,
        coordinator.maximumBatchSize,
        coordinator.hostDispatchNanoseconds,
        coordinator.hostSubmitNanoseconds,
        coordinator.hostCollectNanoseconds,
    };
}

void QemuCaptureCoordinator::registerCaptureWorkerTile(std::uint32_t tileId,
                                                       std::uint32_t workerCount)
{
    CaptureWorkerCoordinator& coordinator = captureWorkerCoordinator();
    const std::lock_guard<std::mutex> lock(coordinator.mutex);
    if (!coordinator.pendingTasks.empty())
    {
        throw std::logic_error("parallel QEMU capture registration began after execution");
    }
    if (coordinator.registeredTiles.empty())
    {
        coordinator.workerCount = workerCount;
    }
    else if (coordinator.workerCount != workerCount)
    {
        throw std::logic_error("parallel QEMU capture configuration differs across tiles");
    }
    if (!coordinator.registeredTiles.insert(tileId).second)
    {
        throw std::logic_error("invalid tile registration in parallel QEMU capture");
    }
}

bool QemuCaptureCoordinator::enqueueCaptureWorkerTask(QemuCaptureExecutor::Task task)
{
    CaptureWorkerCoordinator& coordinator = captureWorkerCoordinator();
    const std::lock_guard<std::mutex> lock(coordinator.mutex);
    const std::thread::id caller = std::this_thread::get_id();
    if (coordinator.registeredTiles.count(task.tileId) == 0)
    {
        throw std::logic_error("parallel QEMU capture is incomplete or unregistered");
    }
    if (!coordinator.ownerThread.has_value())
    {
        coordinator.ownerThread = caller;
    }
    else if (*coordinator.ownerThread != caller)
    {
        throw std::logic_error("parallel QEMU capture crossed SST event threads");
    }
    if (!coordinator.frontierTick.has_value())
    {
        coordinator.frontierTick = task.frontierTick;
    }
    else if (*coordinator.frontierTick != task.frontierTick)
    {
        throw std::logic_error("parallel QEMU capture retained an earlier frontier");
    }
    if (!coordinator.pendingTiles.insert(task.tileId).second)
    {
        throw std::logic_error("tile entered one parallel QEMU capture twice");
    }
    const bool needsDispatch = coordinator.pendingTasks.empty();
    coordinator.pendingTasks.push_back(std::move(task));
    return needsDispatch;
}

std::vector<QemuCaptureExecutor::Completion>
QemuCaptureCoordinator::dispatchCaptureWorker(std::uint64_t frontierTick)
{
    CaptureWorkerCoordinator& coordinator = captureWorkerCoordinator();
    std::vector<QemuCaptureExecutor::Task> tasks;
    {
        const std::lock_guard<std::mutex> lock(coordinator.mutex);
        if (coordinator.pendingTasks.empty())
        {
            return {};
        }
        if (!coordinator.frontierTick.has_value() || *coordinator.frontierTick != frontierTick ||
            !coordinator.ownerThread.has_value() ||
            *coordinator.ownerThread != std::this_thread::get_id())
        {
            throw std::logic_error(
                "parallel QEMU capture dispatch has an invalid frontier or owner");
        }
        tasks = std::move(coordinator.pendingTasks);
        coordinator.pendingTasks.clear();
        coordinator.pendingTiles.clear();
        coordinator.frontierTick.reset();
        ++coordinator.dispatchCount;
        coordinator.capturedTaskCount += tasks.size();
        if (tasks.size() > 1)
        {
            ++coordinator.parallelDispatchCount;
        }
        coordinator.maximumBatchSize = std::max(coordinator.maximumBatchSize, tasks.size());
    }

    const HostElapsed dispatchTime(coordinator.hostDispatchNanoseconds);
    if (tasks.size() == 1)
    {
        QemuCaptureExecutor::Task& task = tasks.front();
        QemuSyncEvent event = task.capture();
        if (event.grantEpoch != task.grantEpoch)
        {
            throw QemuCaptureExecutionError(task.tileId,
                                             "event grant epoch does not match the runtime task");
        }
        if (event.eventSequence == 0)
        {
            throw QemuCaptureExecutionError(task.tileId, "event sequence must be nonzero");
        }
        task.validate(event);
        const std::uint64_t delivery = task.modeledDeliveryTick(event);
        if (delivery < frontierTick)
        {
            throw QemuCaptureExecutionError(task.tileId,
                                             "modeled delivery tick precedes the runtime frontier");
        }
        std::vector<QemuCaptureExecutor::Completion> result;
        result.push_back({
            delivery,
            0,
            task.tileId,
            task.grantEpoch,
            event.eventSequence,
            std::move(event),
            std::move(task.commit),
        });
        return result;
    }

    if (coordinator.executor == nullptr)
    {
        coordinator.executor = std::make_unique<QemuCaptureExecutor>(coordinator.workerCount);
    }
    try
    {
        {
            const HostElapsed submitTime(coordinator.hostSubmitNanoseconds);
            coordinator.executor->begin(frontierTick, tasks.size());
            coordinator.executor->submitBatch(std::move(tasks));
        }
        const HostElapsed collectTime(coordinator.hostCollectNanoseconds);
        return coordinator.executor->collect();
    }
    catch (...)
    {
        if (coordinator.executor->active())
        {
            coordinator.executor->discard();
        }
        throw;
    }
}

namespace
{
std::mutex leaseMutex;
std::unordered_map<std::uint32_t, std::weak_ptr<std::atomic<bool>>> leases;
} // namespace
void QemuCaptureCoordinator::attach(std::uint32_t tile, std::shared_ptr<std::atomic<bool>> lease)
{
    const std::lock_guard<std::mutex> lock(leaseMutex);
    auto previous = leases[tile].lock();
    if (previous && previous->load())
        throw std::logic_error("duplicate live CPU capture endpoint");
    leases[tile] = lease;
}
void QemuCaptureCoordinator::cancel(std::uint32_t tile)
{
    {
        auto& c = captureWorkerCoordinator();
        const std::lock_guard<std::mutex> lock(c.mutex);
        c.pendingTasks.erase(std::remove_if(c.pendingTasks.begin(), c.pendingTasks.end(),
                                            [tile](const auto& task)
                                            { return task.tileId == tile; }),
                             c.pendingTasks.end());
        c.pendingTiles.erase(tile);
        if (c.pendingTasks.empty())
            c.frontierTick.reset();
    }
    const std::lock_guard<std::mutex> lock(leaseMutex);
    leases.erase(tile);
}
QemuSyncEvent QemuCaptureCoordinator::captureHost(SharedSyncMemoryBridge& bridge, int pid,
                                                  std::uint64_t watchdogMilliseconds,
                                                  std::uint64_t spinMicroseconds,
                                                  const std::atomic<bool>& active)
{
    if (pid <= 0)
        throw std::logic_error("ready-set QEMU has no live process ID");
    const auto started = std::chrono::steady_clock::now();
    while (active.load())
    {
        auto event = bridge.waitForEvent(std::chrono::milliseconds(50),
                                         std::chrono::microseconds(spinMicroseconds));
        if (bridge.protocolError())
            throw std::runtime_error("QEMU synchronization bridge protocol error");
        if (event)
        {
            recordQemuCaptureHostTime(*event, std::chrono::steady_clock::now() - started);
            return *event;
        }
        siginfo_t status{};
        int result;
        do
        {
            result = waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != ECHILD)
            throw std::system_error(errno, std::generic_category(),
                                    "waitid failed for ready-set QEMU");
        if (result < 0 || status.si_pid != 0)
            throw std::runtime_error("QEMU exited before its ready-set event");
        if (watchdogMilliseconds && std::chrono::steady_clock::now() - started >=
                                        std::chrono::milliseconds(watchdogMilliseconds))
            throw std::runtime_error("ready-set capture exceeded progress watchdog");
    }
    throw std::runtime_error("QEMU capture cancelled");
}

} // namespace SST::Mittens
