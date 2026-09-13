#pragma once
#include "qemuReadySetExecutor.h"
#include <array>
#include <atomic>
#include <chrono>
namespace SST::Mittens
{
struct RuntimeQemuReadySetStatistics
{
    std::uint32_t reporterTile;
    std::uint64_t dispatchCount;
    std::uint64_t capturedTaskCount;
    std::uint64_t parallelDispatchCount;
    std::size_t maximumBatchSize;
};
struct QemuCaptureHostStatistics
{
    std::array<std::atomic<std::uint64_t>, MITTENS_SYNC_STOP_COUNT> counts{};
    std::array<std::atomic<std::uint64_t>, MITTENS_SYNC_STOP_COUNT>
        nanoseconds{};
    std::array<std::atomic<std::uint64_t>, MITTENS_SYNC_STOP_COUNT>
        maximumNanoseconds{};
    std::array<std::atomic<std::uint64_t>, 8> fencePayloadCounts{};
    std::array<std::atomic<std::uint64_t>, 8> fencePayloadNanoseconds{};
};
struct LocalQemuLookaheadStatistics
{
    std::uint32_t reporterTile;
    std::uint32_t workerCount;
    QemuAsyncCaptureExecutor::Statistics executor;
    std::uint64_t readyAtCommit;
    std::uint64_t waitedAtCommit;
    std::array<std::uint64_t, MITTENS_SYNC_STOP_COUNT> fusedTerminalCounts{};
};
class QemuCaptureCoordinator final
{
  public:
    // The owner revokes the lease, then drains captures before closing bridges.
    static void attach(std::uint32_t tile, std::shared_ptr<std::atomic<bool>> lease);
    static void cancel(std::uint32_t tile);
    static QemuSyncEvent captureHost(SharedSyncMemoryBridge& bridge, int pid,
                                     std::uint64_t watchdogMilliseconds,
                                     std::uint64_t spinMicroseconds,
                                     const std::atomic<bool>& active);
    static QemuCaptureHostStatistics& qemuCaptureHostStatistics();
    static void recordQemuCaptureHostTime(const QemuSyncEvent& event,
                                          std::chrono::steady_clock::duration duration) noexcept;
    static void registerLocalQemuLookaheadTile(std::uint32_t tileId, std::uint32_t workerCount,
                                               std::uint32_t expectedTiles,
                                               const std::string& independenceProof);
    static std::future<QemuSyncEvent>
    submitLocalQemuLookahead(std::uint32_t workerCount, QemuAsyncCaptureExecutor::Capture capture);
    static void recordLocalQemuLookaheadConsume(bool ready) noexcept;
    static void recordLocalQemuLookaheadFusedTerminal(std::uint32_t stopReason) noexcept;
    static LocalQemuLookaheadStatistics localQemuLookaheadStatistics();
    static RuntimeQemuReadySetStatistics runtimeQemuReadySetStatistics();
    static void registerRuntimeQemuReadySetTile(std::uint32_t tileId, std::uint32_t workerCount,
                                                std::uint32_t expectedTiles,
                                                const std::string& independenceProof);
    static void enqueueRuntimeQemuReadySetTask(QemuReadySetExecutor::Task task);
    static std::vector<QemuReadySetExecutor::Completion>
    dispatchRuntimeQemuReadySet(std::uint64_t frontierTick);
    static void registerInitialQemuReadySetTile(std::uint64_t partitionKey, std::uint32_t tileId,
                                                std::uint32_t workerBudget,
                                                std::uint32_t partitionWorkerCount,
                                                std::uint32_t expectedTiles,
                                                const std::string& independenceProof);
    static std::optional<std::vector<QemuReadySetExecutor::Completion>>
    submitInitialQemuReadySetTask(std::uint64_t partitionKey, std::uint32_t workerBudget,
                                  std::uint32_t partitionWorkerCount, std::uint32_t expectedTiles,
                                  QemuReadySetExecutor::Task task);
};
} // namespace SST::Mittens
