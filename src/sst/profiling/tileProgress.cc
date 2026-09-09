#include "tileProgress.h"
#include <sstream>
#include "../execution/diagnosticMessage.h"

namespace SST::Mittens
{

ProgressSnapshot makeTileProgressSnapshot(const TileMeasurementSnapshot& data,
                                         const RxStatus& rx,
                                         const TxStatus& tx,
                                         std::uint64_t pendingMemoryRequests,
                                         std::uint64_t wallMilliseconds, const char* kind)
{
    ProgressSnapshot progress;
    progress.kind = kind;
    progress.wallTimeMilliseconds = wallMilliseconds;
    progress.simulationTick = data.finishTick;
    progress.instructions = data.accounting.total.instructions;
    progress.cpuCycles = data.accounting.totalCycles;
    progress.taskFinishEvents = data.cpu.taskFinishEvents_;
    progress.taskFinishEventsAvailable =
        data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_START] != 0 ||
        data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_FINISH] != 0;
    progress.physicalGlobalDMASubmitted = data.globalDMA.submitted;
    progress.physicalGlobalDMACompleted = data.globalDMA.completed;
    progress.analogCommandsSubmitted = data.analog.submitted;
    progress.analogCommandsCompleted = data.analog.completed;
    progress.localEpochAvailable = data.configuration.epochBarrierEpochs != 0;
    progress.localEpoch = progress.localEpochAvailable ? data.barriers.expectedEpochBarrier_ : UINT32_MAX;
    progress.memoryInitializationComplete = !data.barriers.memoryInitializationPhase_;
    progress.waitReason = data.cpu.activeWaitStartTick_.has_value()
                              ? data.cpu.activeWaitReason_
                              : static_cast<std::uint32_t>(MITTENS_SYNC_STOP_NONE);
    progress.waitTicks = progress.waitReason < data.cpu.waitTicks_.size()
                             ? data.cpu.waitTicks_[progress.waitReason] : 0;
    if (data.cpu.activeWaitStartTick_.has_value() && data.finishTick >= *data.cpu.activeWaitStartTick_)
        progress.waitTicks += data.finishTick - *data.cpu.activeWaitStartTick_;
    progress.networkPackets = data.tx.networkTransmitPackets + data.rx.networkReceivePackets;
    progress.networkWords = data.tx.networkTransmitWords + data.rx.networkReceiveWords;
    progress.networkWordHops = data.tx.networkWordHops;
    progress.networkQueueTicks = data.tx.networkEndpointQueueTicks;
    progress.receiveDMATransfers = data.rx.receiveDMATransfers;
    progress.receiveDMAWords = data.rx.receiveDMAWords;
    progress.receiveDMAActiveCycles = data.rx.receiveDMAActiveCycles;
    progress.pendingNetworkReceives = rx.pendingNetwork + tx.pendingNetwork;
    progress.pendingReceiveDMA = rx.pendingTransfers;
    progress.pendingReceiveDMADescriptors = rx.pendingDescriptors;
    progress.pendingCompletedReceiveFrames = rx.completedFrames;
    progress.pendingReadyReceiveBursts = rx.readyBursts;
    progress.pendingIncomingFrameAssemblies = rx.incomingFrames;
    progress.bridgeReceiveBursts = rx.bridgeReceiveBursts;
    progress.receiveDMAAuthorizationAvailable = rx.authorizationAvailable;
    progress.bridgeHeadSource = rx.bridgeHead.has_value() ? rx.bridgeHead->source : UINT32_MAX;
    progress.bridgeHeadSoftwareVisible = rx.bridgeHead.has_value() && rx.bridgeHead->softwareVisible;
    progress.bridgeHeadScheduled = rx.bridgeHeadScheduled;
    progress.firstUnscheduledPayloadOffset = rx.firstUnscheduledPayloadOffset;
    progress.firstUnscheduledPayloadSource = rx.firstUnscheduledPayloadSource;
    progress.firstUnscheduledPayloadDescriptorCount = rx.firstUnscheduledPayloadDescriptorCount;
    progress.firstUnscheduledPayloadDescriptorRoute = rx.firstUnscheduledPayloadDescriptorRoute;
    progress.firstUnscheduledPayloadDescriptorIteration = rx.firstUnscheduledPayloadDescriptorIteration;
    progress.firstUnscheduledPayloadDescriptorRemainingWords = rx.firstUnscheduledPayloadDescriptorRemainingWords;
    progress.pendingGlobalDMA = data.globalDMA.pending;
    progress.pendingMemory = pendingMemoryRequests +
                            static_cast<std::uint64_t>(data.memory.outstandingMemoryWrites_) +
                            static_cast<std::uint64_t>(data.memory.outstandingMemoryReads_);
    progress.synchronizationEvents = data.cpu.synchronizationEvents_;
    progress.synchronizationGrants = data.cpu.synchronizationGrants_;
    return progress;
}

std::string formatTileProgress(std::uint32_t tileId, const ProgressSnapshot& progress)
{
    std::ostringstream message;
    message.imbue(std::locale::classic());
    message << "MITTENS_PROGRESS tile=" << tileId
            << " kind=" << (progress.kind == nullptr ? "unknown" : progress.kind)
            << " wall_ms=" << progress.wallTimeMilliseconds
            << " sim_tick=" << progress.simulationTick
            << " instructions=" << progress.instructions
            << " task_finishes=" << progress.taskFinishEvents
            << " task_finishes_available=" << progress.taskFinishEventsAvailable
            << " physical_global_dma_submitted=" << progress.physicalGlobalDMASubmitted
            << " physical_global_dma_completed=" << progress.physicalGlobalDMACompleted
            << " analog_commands_submitted=" << progress.analogCommandsSubmitted
            << " analog_commands_completed=" << progress.analogCommandsCompleted
            << " local_epoch=" << progress.localEpoch
            << " local_epoch_available=" << progress.localEpochAvailable
            << " memory_initialization_complete=" << progress.memoryInitializationComplete
            << " wait_reason=" << progress.waitReason
            << " wait_ticks=" << progress.waitTicks
            << " pending_network=" << progress.pendingNetworkReceives
            << " pending_rx_dma=" << progress.pendingReceiveDMA
            << " pending_global_dma=" << progress.pendingGlobalDMA
            << " pending_memory=" << progress.pendingMemory << '\n';
    return message.str();
}

std::string formatProgressWatchdog(std::uint32_t tileId, std::uint64_t timeoutMilliseconds,
                                  std::uint64_t elapsedMilliseconds, std::uint64_t deploymentEpoch,
                                  std::uint64_t initializationEpoch)
{
    return diagnosticMessage("watchdog formatting failed",
        "MITTENS_PROGRESS_WATCHDOG tile=%u timeout_ms=%llu "
        "progress_elapsed_ms=%llu deployment_epoch=%llu initialization_execution_epoch=%llu\n",
        static_cast<unsigned>(tileId), static_cast<unsigned long long>(timeoutMilliseconds),
        static_cast<unsigned long long>(elapsedMilliseconds),
        static_cast<unsigned long long>(deploymentEpoch), static_cast<unsigned long long>(initializationEpoch));
}

} // namespace SST::Mittens
