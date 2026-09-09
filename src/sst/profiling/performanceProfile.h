#ifndef SST_MITTENS_PERFORMANCE_PROFILE_H
#define SST_MITTENS_PERFORMANCE_PROFILE_H

#include "summarySnapshot.h"
#include "../memory/scratchpad/scratchpadTimingModel.h"

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <limits>
#include <string>

namespace SST
{
namespace Mittens
{

struct ProgressSnapshot final
{
    const char* kind = "periodic";
    std::uint64_t wallTimeMilliseconds = 0;
    std::uint64_t simulationTick = 0;
    std::uint64_t instructions = 0;
    std::uint64_t cpuCycles = 0;
    std::uint64_t taskFinishEvents = 0;
    // These counters are observation points, not inferred architectural
    // state.  A summary/off guest does not emit task-boundary events, and an
    // exact-dependency deployment does not instantiate the host epoch
    // barrier.  Keep availability explicit so a zero or UINT32_MAX cannot be
    // mistaken for a stalled deployment.
    std::uint32_t taskFinishEventsAvailable = 0;
    std::uint64_t physicalGlobalDMASubmitted = 0;
    std::uint64_t physicalGlobalDMACompleted = 0;
    std::uint64_t analogCommandsSubmitted = 0;
    std::uint64_t analogCommandsCompleted = 0;
    std::uint32_t localEpoch = UINT32_MAX;
    std::uint32_t localEpochAvailable = 0;
    std::uint32_t memoryInitializationComplete = 0;
    std::uint32_t waitReason = 0;
    std::uint64_t waitTicks = 0;
    std::uint64_t networkPackets = 0;
    std::uint64_t networkWords = 0;
    std::uint64_t networkWordHops = 0;
    std::uint64_t networkQueueTicks = 0;
    std::uint64_t receiveDMATransfers = 0;
    std::uint64_t receiveDMAWords = 0;
    std::uint64_t receiveDMAActiveCycles = 0;
    std::uint64_t pendingNetworkReceives = 0;
    std::uint64_t pendingReceiveDMA = 0;
    std::uint64_t pendingReceiveDMADescriptors = 0;
    std::uint64_t pendingCompletedReceiveFrames = 0;
    std::uint64_t pendingReadyReceiveBursts = 0;
    std::uint64_t pendingIncomingFrameAssemblies = 0;
    std::uint64_t bridgeReceiveBursts = 0;
    std::uint32_t receiveDMAAuthorizationAvailable = 0;
    std::uint32_t bridgeHeadSource = UINT32_MAX;
    std::uint32_t bridgeHeadSoftwareVisible = 0;
    std::uint32_t bridgeHeadScheduled = 0;
    std::uint32_t firstUnscheduledPayloadOffset = UINT32_MAX;
    std::uint32_t firstUnscheduledPayloadSource = UINT32_MAX;
    std::uint32_t firstUnscheduledPayloadDescriptorCount = 0;
    std::uint32_t firstUnscheduledPayloadDescriptorRoute = UINT32_MAX;
    std::uint64_t firstUnscheduledPayloadDescriptorIteration = UINT64_MAX;
    std::uint32_t firstUnscheduledPayloadDescriptorRemainingWords = 0;
    std::uint64_t pendingGlobalDMA = 0;
    std::uint64_t pendingMemory = 0;
    std::uint64_t synchronizationEvents = 0;
    std::uint64_t synchronizationGrants = 0;
};

class PerformanceProfile final
{
  public:
    PerformanceProfile() = default;
    ~PerformanceProfile() = default;

    PerformanceProfile(const PerformanceProfile&) = delete;
    PerformanceProfile& operator=(const PerformanceProfile&) = delete;
    PerformanceProfile(PerformanceProfile&&) = delete;
    PerformanceProfile& operator=(PerformanceProfile&&) = delete;

    void configure(std::uint32_t tileId, const std::string& outputDirectory, bool traceEnabled);

    bool enabled() const noexcept
    {
        return enabled_;
    }
    bool traceEnabled() const noexcept
    {
        return traceEnabled_;
    }

    void recordWait(std::uint64_t startTick, std::uint64_t finishTick, const char* reason,
                    std::uint64_t eventSequence);
    void recordNetwork(const char* event, std::uint64_t packetId, std::uint32_t source,
                       std::uint32_t destination, std::uint32_t routeId, std::uint64_t executionId,
                       std::uint64_t logicalIteration, const char* kind, std::uint32_t words,
                       std::uint32_t protocolWords, std::uint32_t payloadWords, std::uint32_t hops,
                       std::uint64_t readyTick, std::uint64_t injectionTick,
                       std::uint64_t eventTick);
    void recordReceiveDMA(const char* event, std::uint32_t source, std::uint32_t routeId,
                          std::uint64_t executionId, std::uint64_t logicalIteration,
                          std::uint32_t burstIndex, std::uint32_t words, std::uint64_t scheduleTick,
                          std::uint64_t startCycle, std::uint64_t completionCycle,
                          std::uint64_t eventTick, std::uint64_t serviceCycles,
                          std::uint32_t invalidationLines = 0);
    void recordAnalog(const char* phase, std::uint64_t ticket, std::uint32_t operation,
                      std::uint32_t arrayId, std::uint64_t deviceCycle, std::uint64_t eventTick);
    void recordMemory(const char* event, std::uint64_t requestId, std::uint64_t guestAddress,
                      std::uint64_t timingAddress, std::uint64_t programCounter,
                      std::uint64_t returnAddress, std::uint32_t size, bool write,
                      std::uint32_t taskId, std::uint64_t executionId, const char* phase,
                      std::uint64_t issueTick, std::uint64_t eventTick);
    void recordTransmitBlocked(std::uint64_t eventSequence, std::uint32_t routeId,
                               std::uint64_t executionId, std::uint32_t destination,
                               const char* kind, std::uint32_t words, std::uint64_t startTick,
                               std::uint64_t finishTick, std::uint64_t retryCount,
                               std::uint32_t maximumQueueOccupancy);
    void recordProgressSnapshot(const ProgressSnapshot& snapshot);
    void recordScratchpadBeat(const ScratchpadBeatObservation& beat);
    void recordReceiveBoundary(const char* event, std::uint32_t source,
                               std::uint32_t route, std::uint64_t execution,
                               std::uint64_t iteration, std::uint32_t words,
                               std::uint64_t tick);
    void writeSummary(const SummarySnapshot& snapshot);

  private:
    std::ofstream& open(std::ofstream& stream, const std::string& suffix, const char* header);
    std::string path(const std::string& suffix) const;

    std::uint32_t tileId_ = 0;
    std::string outputDirectory_;
    bool enabled_ = false;
    bool traceEnabled_ = false;
    std::ofstream waitStream_;
    std::ofstream networkStream_;
    std::ofstream receiveDMAStream_;
    std::ofstream analogStream_;
    std::ofstream memoryStream_;
    std::ofstream transmitBlockedStream_;
    std::ofstream progressStream_;
    std::ofstream scratchpadBeatStream_;
    std::ofstream receiveBoundaryStream_;
    std::uint64_t clockRegressionCount_ = 0;
    std::uint64_t progressSnapshotCount_ = 0;
    std::uint64_t progressWatchdogCount_ = 0;
    std::uint64_t progressCounterFlushCount_ = 0;
};

// The shared controller owns exact-readiness state, so its progress cannot be
// inferred from per-tile pending tokens.  This bounded snapshot is written on
// semantic checkpoints and at a wall-time cadence.  Its two equalities make
// dropped or double-counted requests/releases fail closed before evidence is
// persisted.
struct GlobalRAMProgressSnapshot final
{
    const char* kind = "periodic";
    std::uint64_t wallTimeMilliseconds = 0;
    std::uint64_t simulationCycle = 0;
    std::uint64_t physicalDMASubmitted = 0;
    std::uint64_t physicalDMACompleted = 0;
    std::uint64_t physicalDMABytesCompleted = 0;
    std::uint64_t readinessBlocked = 0;
    std::uint64_t readinessReleased = 0;
    std::uint64_t readinessCurrentlyBlocked = 0;
    std::uint64_t readinessPublications = 0;
    std::uint64_t queuedRequests = 0;
    std::uint64_t activeRequests = 0;

    bool reconciles() const noexcept;
};

// One immutable controller-owned record for every completed physical DMA.
// These four timestamps are the Class-A equivalence contract: a host transport
// optimization is timing-faithful only when the complete request timeline is
// identical, not merely its aggregate request and byte counts.
struct GlobalRAMRequestTimeline final
{
    std::uint64_t requestSequence = 0;
    std::uint32_t tileId = 0;
    std::uint64_t executionId = 0;
    std::uint32_t tokenId = 0;
    std::uint64_t logicalIteration = 0;
    std::uint64_t globalOffset = 0;
    std::uint64_t scratchpadOffset = 0;
    std::uint32_t byteCount = 0;
    std::uint32_t requestFlags = 0;
    bool write = false;
    std::uint64_t arrivalCycle = 0;
    std::uint64_t readinessCycle = 0;
    std::uint64_t serviceStartCycle = 0;
    std::uint64_t completionCycle = 0;
    std::uint64_t serviceCycles = 0;

    bool reconciles() const noexcept;
};

// Exact-execution teardown is a controller rendezvous rather than a physical
// DMA, so it must not be folded into the request timeline.  Record one row per
// active tile from controller arrival through controller release.
struct GlobalRAMTeardownTimeline final
{
    std::uint64_t executionId = 0;
    std::uint32_t tileId = 0;
    std::uint64_t arrivalCycle = 0;
    std::uint64_t releaseCycle = 0;

    bool reconciles() const noexcept;
};

class GlobalRAMPerformanceProfile final
{
  public:
    GlobalRAMPerformanceProfile() = default;
    ~GlobalRAMPerformanceProfile() = default;

    GlobalRAMPerformanceProfile(const GlobalRAMPerformanceProfile&) = delete;
    GlobalRAMPerformanceProfile& operator=(const GlobalRAMPerformanceProfile&) = delete;

    void configure(const std::string& outputDirectory);
    bool enabled() const noexcept
    {
        return enabled_;
    }
    void recordProgressSnapshot(const GlobalRAMProgressSnapshot& snapshot);
    void recordRequestTimeline(const GlobalRAMRequestTimeline& timeline);
    void recordTeardownTimeline(const GlobalRAMTeardownTimeline& timeline);
    bool requestTotalsReconcile(std::uint64_t requestCount, std::uint64_t readinessDelayCycles,
                                std::uint64_t queueDelayCycles,
                                std::uint64_t serviceCycles) const noexcept;
    bool teardownTotalsReconcile(std::uint64_t teardownCount,
                                 std::uint64_t teardownWaitCycles) const noexcept;

  private:
    std::string path(const char* suffix) const;

    std::string outputDirectory_;
    bool enabled_ = false;
    std::ofstream progressStream_;
    std::ofstream requestStream_;
    std::ofstream teardownStream_;
    std::uint64_t requestCount_ = 0;
    std::uint64_t readinessDelayCycles_ = 0;
    std::uint64_t queueDelayCycles_ = 0;
    std::uint64_t serviceCycles_ = 0;
    std::uint64_t teardownCount_ = 0;
    std::uint64_t teardownWaitCycles_ = 0;
};

struct MemoryInitializationBarrierTimeline final
{
    std::uint32_t tileId = 0;
    std::uint64_t arrivalCycle = 0;
    std::uint64_t releaseCycle = 0;

    bool reconciles() const noexcept;
};

class MemoryInitializationBarrierPerformanceProfile final
{
  public:
    MemoryInitializationBarrierPerformanceProfile() = default;
    ~MemoryInitializationBarrierPerformanceProfile() = default;

    MemoryInitializationBarrierPerformanceProfile(
        const MemoryInitializationBarrierPerformanceProfile&) = delete;
    MemoryInitializationBarrierPerformanceProfile&
    operator=(const MemoryInitializationBarrierPerformanceProfile&) = delete;

    void configure(const std::string& outputDirectory);
    bool enabled() const noexcept
    {
        return enabled_;
    }
    void record(const MemoryInitializationBarrierTimeline& timeline);
    bool totalsReconcile(std::uint64_t tileCount, std::uint64_t tileWaitCycles,
                         std::uint64_t barrierWaitCycles) const noexcept;

  private:
    std::string path() const;

    std::string outputDirectory_;
    bool enabled_ = false;
    std::ofstream stream_;
    std::uint64_t tileCount_ = 0;
    std::uint64_t tileWaitCycles_ = 0;
    std::uint64_t firstArrivalCycle_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lastReleaseCycle_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
