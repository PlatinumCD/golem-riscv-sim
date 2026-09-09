#ifndef SST_MITTENS_GLOBAL_RAM_CONTROLLER_H
#define SST_MITTENS_GLOBAL_RAM_CONTROLLER_H

#include "globalDMAEvent.h"
#include "globalRAMReadiness.h"
#include "../configuration/globalRAMConfiguration.h"
#include "../profiling/performanceProfile.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>

namespace SST {
namespace Mittens {

class GlobalRAMController final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        GlobalRAMController,
        "mittens",
        "globalRAMController",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Sparse shared global RAM DMA controller",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        {"capacity_bytes", "Sparse global RAM capacity", "34359738368"},
        {"clock", "Controller arbitration clock", "1GHz"},
        {"tile_count", "Number of addressable tile IDs", "1"},
        {"active_tiles", "Array of active tile IDs; omitted means every tile", ""},
        {"channels", "Independent RAM service channels", "1"},
        {"queue_depth", "Total finite request queue depth", "16"},
        {"per_tile_queue_depth", "Finite queue depth per tile", "8"},
        {"setup_cycles", "Setup cycles per request", "8"},
        {"bytes_per_cycle", "Transfer bandwidth per channel", "32"},
        {"burst_bytes", "RAM burst size", "64"},
        {"fixed_latency_cycles", "Latency charged per burst", "2"},
        {"maximum_request_bytes", "Maximum bytes in one physical DMA request", "4294967295"},
        {"demand_write_burst", "Maximum consecutive writes that advance blocked reads before one other ready request is forced; zero disables demand priority", "0"},
        {"read_priority_burst", "Maximum consecutive ready reads selected before one ready write is forced; zero preserves round-robin scheduling", "0"},
        {"reserved_read_channels", "RAM channels withheld from writes so newly ready reads can begin service; zero disables reservation", "0"},
        {"dependency_mode", "RAM readiness policy: bulk_barrier or exact_dependencies", "bulk_barrier"},
        {"profile_output_directory", "Directory for bounded controller progress snapshots", ""},
        {"progress_snapshot_interval_ms", "Wall-time interval for controller progress snapshots; zero disables periodic snapshots", "10000"},
        {"verbose", "Controller diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"dma%(tile)d", "Bidirectional tile DMA link", {"mittens.GlobalDMAEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"requests", "Completed global DMA requests", "requests", 1},
        {"bytes", "Completed global DMA bytes", "bytes", 1},
        {"readiness_delay_cycles", "Delay from request arrival until exact data readiness", "cycles", 1},
        {"queue_delay_cycles", "Ready request delay awaiting a RAM service channel", "cycles", 1},
        {"service_cycles", "RAM transfer service", "cycles", 1},
        {"maximum_queue_occupancy", "Maximum queued requests", "requests", 1},
        {"readiness_blocked_reads", "Exact reads blocked awaiting complete committed coverage", "requests", 1},
        {"readiness_releases", "Blocked exact reads released by publication", "requests", 1},
        {"readiness_interval_lookups", "Committed-interval coverage lookups", "lookups", 1},
        {"readiness_publications", "Exact write intervals committed", "publications", 1},
        {"readiness_duplicate_publications", "Rejected duplicate or overlapping exact publications", "publications", 1},
        {"readiness_maximum_waiters", "Maximum simultaneously blocked exact reads", "requests", 1},
        {"readiness_execution_teardowns", "Completed exact execution teardown rendezvous", "executions", 1},
        {"execution_teardown_wait_cycles", "Per-tile wait from exact teardown arrival through controller release", "cycles", 1},
        {"demand_write_waiter_links", "Blocked-read overlaps attached to queued producer writes", "links", 1},
        {"demand_write_promotions", "Queued writes promoted from background to blocked-read demand", "requests", 1},
        {"demand_write_selections", "Demand producer writes selected for RAM service", "requests", 1})

    GlobalRAMController(SST::ComponentId_t id, SST::Params& params);
    ~GlobalRAMController() override;

    void setup() override;
    void finish() override;
    void emergencyShutdown() override;

  private:
    enum class DependencyMode {
        BulkBarrier,
        ExactDependencies,
    };
    struct PendingRequest {
        GlobalDMAEvent* event;
        std::uint64_t arrivalCycle;
        std::uint64_t readinessCycle;
        std::uint64_t requestSequence;
        bool schedulable;
        std::uint32_t demandingWaiters;
    };
    struct ActiveRequest {
        GlobalDMAEvent* event = nullptr;
        std::uint64_t requestSequence = 0;
        std::uint64_t arrivalCycle = 0;
        std::uint64_t readinessCycle = 0;
        std::uint64_t serviceStartCycle = 0;
        std::uint64_t completionCycle = 0;
        std::uint64_t serviceCycles = 0;
    };
    struct ExactExecutionState {
        GlobalRAMCommittedRanges committedRanges;
        std::multimap<std::uint64_t, PendingRequest*> waitersByBegin;
        std::multimap<std::uint64_t, PendingRequest*> queuedWritesByBegin;
        std::vector<GlobalDMAEvent*> teardownEvents;
        std::vector<std::uint64_t> teardownArrivalCycles;
        std::uint32_t teardownCount = 0;
    };
    struct ReadySelection {
        std::uint32_t tile = UINT32_MAX;
        PendingRequest* request = nullptr;
    };

    void handleRequest(SST::Event* event);
    void handleWake(SST::Event* event);
    bool clockTick(SST::Cycle_t cycle);
    void ensureClock();
    bool sleepUntilNextCompletion(std::uint64_t cycle);
    ReadySelection selectReadyRequest();
    ReadySelection selectReadyRequestByDirection(
        GlobalDMADirection direction);
    ReadySelection selectReadyRequestByDemand(bool demanded);
    std::uint64_t serviceCycles(std::uint32_t byteCount) const;
    ExactExecutionState& exactExecution(std::uint64_t executionId);
    bool isExactDataRequest(const GlobalDMAEvent& event) const noexcept;
    bool isEpochZeroRead(const GlobalDMAEvent& event) const noexcept;
    bool isTeardown(const GlobalDMAEvent& event) const noexcept;
    bool requestIsSchedulable(GlobalDMAEvent& event);
    void publishCompletedWrite(const GlobalDMAEvent& event);
    void releaseCoveredReads(
        std::uint64_t executionId, std::uint64_t publicationBegin);
    std::uint32_t blockedReadDemandCount(
        const ExactExecutionState& execution,
        const GlobalDMAEvent& write);
    void adjustQueuedWriteDemand(
        ExactExecutionState& execution,
        const GlobalDMAEvent& read,
        bool increment);
    void indexQueuedWrite(ExactExecutionState& execution,
                          PendingRequest& request);
    void unindexQueuedWrite(PendingRequest& request);
    void acceptTeardown(GlobalDMAEvent* event);
    void tryCompleteTeardown(
        std::uint64_t executionId, std::uint64_t releaseCycle);
    bool executionHasDataRequests(std::uint64_t executionId) const noexcept;
    bool executionHasActiveRequest(std::uint64_t executionId) const noexcept;
    bool hasSchedulableQueuedRequest() const noexcept;
    bool hasFullyArrivedTeardown(std::uint64_t executionId) const noexcept;
    std::uint64_t activeRequestCount() const noexcept;
    std::uint32_t activeWriteCount() const noexcept;
    bool directionIsAdmissible(GlobalDMADirection direction) const noexcept;
    void recordProgressSnapshot(const char* kind);
    void maybeRecordProgressSnapshot(const char* kind, bool force = false);

    const GlobalRAMConfiguration configuration_;
    SST::Output output_;
    std::uint64_t capacityBytes_;
    std::uint32_t tileCount_;
    std::uint32_t channelCount_;
    std::uint32_t queueDepth_;
    std::uint32_t perTileQueueDepth_;
    std::uint64_t setupCycles_;
    std::uint32_t bytesPerCycle_;
    std::uint32_t burstBytes_;
    std::uint64_t fixedLatencyCycles_;
    std::uint32_t maximumRequestBytes_ = UINT32_MAX;
    std::uint32_t demandWriteBurstLimit_ = 0;
    std::uint32_t consecutiveDemandWrites_ = 0;
    std::uint32_t readPriorityBurstLimit_ = 0;
    std::uint32_t consecutivePriorityReads_ = 0;
    std::uint32_t reservedReadChannels_ = 0;
    std::uint64_t progressSnapshotIntervalMilliseconds_ = 10000;
    DependencyMode dependencyMode_ = DependencyMode::BulkBarrier;
    GlobalRAMPerformanceProfile performanceProfile_;
    std::vector<SST::Link*> links_;
    std::vector<std::list<PendingRequest>> queues_;
    std::vector<ActiveRequest> channels_;
    std::uint32_t roundRobinCursor_ = 0;
    std::vector<bool> activeTiles_;
    std::uint32_t activeTileCount_ = 0;
    std::uint32_t queuedRequests_ = 0;
    std::uint64_t blockedWaiters_ = 0;
    std::uint64_t maximumWaiters_ = 0;
    std::uint64_t physicalDMASubmitted_ = 0;
    std::uint64_t physicalDMACompleted_ = 0;
    std::uint64_t physicalDMABytesCompleted_ = 0;
    std::uint64_t readinessBlocked_ = 0;
    std::uint64_t readinessReleased_ = 0;
    std::uint64_t readinessPublications_ = 0;
    std::uint64_t readinessDelayCycles_ = 0;
    std::uint64_t queueDelayCycles_ = 0;
    std::uint64_t serviceCycles_ = 0;
    std::uint64_t teardownCompletionCount_ = 0;
    std::uint64_t executionTeardownWaitCycles_ = 0;
    std::unordered_map<std::uint64_t, ExactExecutionState> exactExecutions_;
    std::unordered_set<std::uint64_t> closedExecutions_;
    int backingDescriptor_ = -1;

    SST::Statistics::Statistic<std::uint64_t>* requestStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* byteStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* readinessDelayStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* queueDelayStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* serviceStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* maximumQueueStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* blockedReadStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* readinessReleaseStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* intervalLookupStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* publicationStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* duplicatePublicationStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* maximumWaiterStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* executionTeardownStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* executionTeardownWaitStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* demandWriteWaiterLinkStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* demandWritePromotionStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* demandWriteSelectionStatistic_ = nullptr;
    std::uint64_t maximumQueueOccupancy_ = 0;
    SST::TimeConverter clockTimeBase_;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
    SST::Link* wakeLink_ = nullptr;
    bool clockRegistered_ = false;
    std::uint64_t scheduledWakeCycle_ = 0;
    std::chrono::steady_clock::time_point lastProgressSnapshotWallTime_{};
    bool activitySnapshotRecorded_ = false;
    bool blockedSnapshotRecorded_ = false;
    bool releaseSnapshotRecorded_ = false;
};

} // namespace Mittens
} // namespace SST

#endif
