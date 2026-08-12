#ifndef SST_MITTENS_PERFORMANCE_PROFILE_H
#define SST_MITTENS_PERFORMANCE_PROFILE_H

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>

namespace SST {
namespace Mittens {

class PerformanceProfile final
{
  public:
    PerformanceProfile() = default;
    ~PerformanceProfile() = default;

    PerformanceProfile(const PerformanceProfile&) = delete;
    PerformanceProfile& operator=(const PerformanceProfile&) = delete;
    PerformanceProfile(PerformanceProfile&&) = delete;
    PerformanceProfile& operator=(PerformanceProfile&&) = delete;

    void configure(std::uint32_t tileId,
                   const std::string& outputDirectory,
                   bool traceEnabled);

    bool enabled() const noexcept { return enabled_; }
    bool traceEnabled() const noexcept { return traceEnabled_; }

    void recordWait(std::uint64_t startTick,
                    std::uint64_t finishTick,
                    const char* reason,
                    std::uint64_t eventSequence);
    void recordNetwork(const char* event,
                       std::uint64_t packetId,
                       std::uint32_t source,
                       std::uint32_t destination,
                       std::uint32_t routeId,
                       std::uint64_t executionId,
                       const char* kind,
                       std::uint32_t words,
                       std::uint32_t protocolWords,
                       std::uint32_t payloadWords,
                       std::uint32_t hops,
                       std::uint64_t readyTick,
                       std::uint64_t injectionTick,
                       std::uint64_t eventTick);
    void recordReceiveDMA(const char* event,
                          std::uint32_t source,
                          std::uint32_t routeId,
                          std::uint64_t executionId,
                          std::uint32_t burstIndex,
                          std::uint32_t words,
                          std::uint64_t scheduleTick,
                          std::uint64_t startCycle,
                          std::uint64_t completionCycle,
                          std::uint64_t eventTick,
                          std::uint64_t serviceCycles,
                          std::uint32_t invalidationLines = 0);
    void recordAnalog(const char* phase,
                      std::uint64_t ticket,
                      std::uint32_t operation,
                      std::uint32_t arrayId,
                      std::uint64_t deviceCycle,
                      std::uint64_t eventTick);
    void recordMemory(const char* event,
                      std::uint64_t requestId,
                      std::uint64_t guestAddress,
                      std::uint64_t timingAddress,
                      std::uint64_t programCounter,
                      std::uint64_t returnAddress,
                      std::uint32_t size,
                      bool write,
                      std::uint32_t taskId,
                      std::uint64_t executionId,
                      const char* phase,
                      std::uint64_t issueTick,
                      std::uint64_t eventTick);
    void recordTransmitBlocked(std::uint64_t eventSequence,
                               std::uint32_t routeId,
                               std::uint64_t executionId,
                               std::uint32_t destination,
                               const char* kind,
                               std::uint32_t words,
                               std::uint64_t startTick,
                               std::uint64_t finishTick,
                               std::uint64_t retryCount,
                               std::uint32_t maximumQueueOccupancy);
    void writeSummary(std::uint64_t finishTick,
                      std::uint64_t instructions,
                      std::uint64_t vectorInstructions,
                      std::uint64_t cpuCycles,
                      std::uint64_t networkPackets,
                      std::uint64_t networkWords,
                      std::uint64_t networkWordHops,
                      std::uint64_t networkTransitTicks,
                      std::uint64_t networkQueueTicks,
                      std::uint64_t analogActiveCycles,
                      std::uint64_t analogLinkBeats,
                      std::uint64_t receiveDMAActiveCycles,
                      std::uint64_t transmitBlockedTicks,
                      std::uint64_t transmitBlockedEvents,
                      std::uint64_t transmitBlockedRetries,
                      std::uint64_t transmitMaximumQueueOccupancy,
                      std::uint64_t maximumOutstandingMemoryRequests,
                      std::uint64_t maximumOutstandingMemoryReads,
                      std::uint64_t maximumStoreBufferOccupancy,
                      std::uint64_t storeBufferFullEvents,
                      std::uint64_t vectorMemoryRequestGroups,
                      std::uint64_t vectorMemoryGroupRequests,
                      std::uint64_t scalarMemoryRequestGroups,
                      std::uint64_t scalarMemoryGroupRequests,
                      const std::uint64_t* waitTicks,
                      const char* const* waitReasonNames,
                      std::size_t waitTickCount);

  private:
    std::ofstream& open(std::ofstream& stream,
                        const std::string& suffix,
                        const char* header);
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
    std::uint64_t clockRegressionCount_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
