#include "performanceProfile.h"
#include "measurementWriter.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace SST
{
namespace Mittens
{

void PerformanceProfile::configure(std::uint32_t tileId, const std::string& outputDirectory,
                                   bool traceEnabled)
{
    tileId_ = tileId;
    outputDirectory_ = outputDirectory;
    enabled_ = !outputDirectory_.empty();
    traceEnabled_ = enabled_ && traceEnabled;
    if (!enabled_)
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(outputDirectory_, error);
    if (error)
    {
        throw std::runtime_error("could not create performance profile directory '" +
                                 outputDirectory_ + "': " + error.message());
    }
}

void PerformanceProfile::recordWait(std::uint64_t startTick, std::uint64_t finishTick,
                                    const char* reason, std::uint64_t eventSequence)
{
    if (!traceEnabled_)
    {
        return;
    }
    if (finishTick < startTick)
    {
        ++clockRegressionCount_;
        finishTick = startTick;
    }
    std::ofstream& output =
        open(waitStream_, "waits.csv",
             "tile_id,event_sequence,reason,start_tick,finish_tick,duration_ticks\n");
    output << tileId_ << ',' << eventSequence << ',' << reason << ',' << startTick << ','
           << finishTick << ',' << (finishTick - startTick) << '\n';
}

void PerformanceProfile::recordNetwork(const char* event, std::uint64_t packetId,
                                       std::uint32_t source, std::uint32_t destination,
                                       std::uint32_t routeId, std::uint64_t executionId,
                                       std::uint64_t logicalIteration, const char* kind,
                                       std::uint32_t words, std::uint32_t protocolWords,
                                       std::uint32_t payloadWords, std::uint32_t hops,
                                       std::uint64_t readyTick, std::uint64_t injectionTick,
                                       std::uint64_t eventTick)
{
    if (!traceEnabled_)
    {
        return;
    }
    std::ofstream& output =
        open(networkStream_, "network.csv",
             "tile_id,event,packet_id,source,destination,route_id,execution_id,"
             "logical_iteration,"
             "kind,words,protocol_words,payload_words,hops,word_hops,ready_tick,"
             "injection_tick,event_tick,endpoint_queue_ticks,transit_ticks\n");
    const std::uint64_t queueTicks = injectionTick >= readyTick ? injectionTick - readyTick : 0;
    const std::uint64_t transitTicks = eventTick >= injectionTick ? eventTick - injectionTick : 0;
    output << tileId_ << ',' << event << ',' << packetId << ',' << source << ',' << destination
           << ',' << routeId << ',' << executionId << ',' << logicalIteration << ',' << kind << ','
           << words << ',' << protocolWords << ',' << payloadWords << ',' << hops << ','
           << static_cast<std::uint64_t>(words) * hops << ',' << readyTick << ',' << injectionTick
           << ',' << eventTick << ',' << queueTicks << ',' << transitTicks << '\n';
}

void PerformanceProfile::recordReceiveDMA(const char* event, std::uint32_t source,
                                          std::uint32_t routeId, std::uint64_t executionId,
                                          std::uint64_t logicalIteration, std::uint32_t burstIndex,
                                          std::uint32_t words, std::uint64_t scheduleTick,
                                          std::uint64_t startCycle, std::uint64_t completionCycle,
                                          std::uint64_t eventTick, std::uint64_t serviceCycles,
                                          std::uint32_t invalidationLines)
{
    if (!traceEnabled_)
    {
        return;
    }
    std::ofstream& output = open(receiveDMAStream_, "receive-dma.csv",
                                 "tile_id,event,source,route_id,execution_id,logical_iteration,"
                                 "burst_index,words,"
                                 "schedule_tick,dma_start_cycle,dma_completion_cycle,event_tick,"
                                 "service_cycles,invalidation_lines\n");
    output << tileId_ << ',' << event << ',' << source << ',' << routeId << ',' << executionId
           << ',' << logicalIteration << ',' << burstIndex << ',' << words << ',' << scheduleTick
           << ',' << startCycle << ',' << completionCycle << ',' << eventTick << ','
           << serviceCycles << ',' << invalidationLines << '\n';
}

void PerformanceProfile::recordAnalog(const char* phase, std::uint64_t ticket,
                                      std::uint32_t operation, std::uint32_t arrayId,
                                      std::uint64_t deviceCycle, std::uint64_t eventTick)
{
    if (!traceEnabled_)
    {
        return;
    }
    std::ofstream& output =
        open(analogStream_, "analog.csv",
             "tile_id,ticket,operation,array_id,phase,device_cycle,event_tick\n");
    output << tileId_ << ',' << ticket << ',' << operation << ',' << arrayId << ',' << phase << ','
           << deviceCycle << ',' << eventTick << '\n';
}

void PerformanceProfile::recordScratchpadBeat(const ScratchpadBeatObservation& b)
{
    if (!traceEnabled_) return;
    auto& out = open(scratchpadBeatStream_, "scratchpad-beats.csv",
        "tile_id,client,direction,offset,bytes,bank,port,issue_cycle,service_cycle,completion_cycle\n");
    out << tileId_ << ',' << b.client << ',' << (b.write ? "write" : "read") << ','
        << b.offset << ',' << b.bytes << ',' << b.bank << ',' << b.port << ','
        << b.issueCycle << ',' << b.serviceCycle << ',' << b.completionCycle << '\n';
}

void PerformanceProfile::recordReceiveBoundary(const char* event, std::uint32_t source,
    std::uint32_t route, std::uint64_t execution, std::uint64_t iteration,
    std::uint32_t words, std::uint64_t tick)
{
    if (!traceEnabled_) return;
    auto& out = open(receiveBoundaryStream_, "receive-boundaries.csv",
        "tile_id,event,source,route_id,execution_id,logical_iteration,words,event_tick\n");
    out << tileId_ << ',' << event << ',' << source << ',' << route << ',' << execution
        << ',' << iteration << ',' << words << ',' << tick << '\n';
}

void PerformanceProfile::recordMemory(const char* event, std::uint64_t requestId,
                                      std::uint64_t guestAddress, std::uint64_t timingAddress,
                                      std::uint64_t programCounter, std::uint64_t returnAddress,
                                      std::uint32_t size, bool write, std::uint32_t taskId,
                                      std::uint64_t executionId, const char* phase,
                                      std::uint64_t issueTick, std::uint64_t eventTick)
{
    if (!traceEnabled_)
    {
        return;
    }
    std::ofstream& output =
        open(memoryStream_, "memory.csv",
             "tile_id,event,request_id,address,timing_address,guest_pc,guest_ra,"
             "size,direction,"
             "task_id,execution_id,phase,issue_tick,event_tick,latency_ticks\n");
    output << tileId_ << ',' << event << ',' << requestId << ',' << guestAddress << ','
           << timingAddress << ',' << programCounter << ',' << returnAddress << ',' << size << ','
           << (write ? "write" : "read") << ',' << taskId << ',' << executionId << ',' << phase
           << ',' << issueTick << ',' << eventTick << ','
           << (eventTick >= issueTick ? eventTick - issueTick : 0) << '\n';
}

void PerformanceProfile::recordTransmitBlocked(std::uint64_t eventSequence, std::uint32_t routeId,
                                               std::uint64_t executionId, std::uint32_t destination,
                                               const char* kind, std::uint32_t words,
                                               std::uint64_t startTick, std::uint64_t finishTick,
                                               std::uint64_t retryCount,
                                               std::uint32_t maximumQueueOccupancy)
{
    if (!traceEnabled_)
    {
        return;
    }
    if (finishTick < startTick)
    {
        ++clockRegressionCount_;
        finishTick = startTick;
    }
    std::ofstream& output = open(transmitBlockedStream_, "transmit-blocked.csv",
                                 "tile_id,event_sequence,route_id,execution_id,destination,kind,"
                                 "words,start_tick,finish_tick,duration_ticks,retry_count,"
                                 "maximum_queue_occupancy\n");
    output << tileId_ << ',' << eventSequence << ',' << routeId << ',' << executionId << ','
           << destination << ',' << kind << ',' << words << ',' << startTick << ',' << finishTick
           << ',' << (finishTick - startTick) << ',' << retryCount << ',' << maximumQueueOccupancy
           << '\n';
}

void PerformanceProfile::recordProgressSnapshot(const ProgressSnapshot& snapshot)
{
    if (!enabled_)
    {
        return;
    }
    if (snapshot.physicalGlobalDMACompleted > snapshot.physicalGlobalDMASubmitted ||
        snapshot.analogCommandsCompleted > snapshot.analogCommandsSubmitted)
    {
        throw std::logic_error("tile progress completion counters exceed submissions");
    }
    if (snapshot.taskFinishEventsAvailable > 1 || snapshot.localEpochAvailable > 1 ||
        snapshot.memoryInitializationComplete > 1 ||
        (snapshot.taskFinishEventsAvailable == 0 && snapshot.taskFinishEvents != 0) ||
        (snapshot.localEpochAvailable == 0 && snapshot.localEpoch != UINT32_MAX))
    {
        throw std::logic_error("tile progress availability metadata is inconsistent");
    }
    std::ofstream& output =
        open(progressStream_, "progress.csv",
             "tile_id,kind,wall_time_ms,simulation_tick,instructions,cpu_cycles,"
             "task_finish_events,task_finish_events_available,"
             "physical_global_dma_submitted,"
             "physical_global_dma_completed,analog_commands_submitted,"
             "analog_commands_completed,local_epoch,local_epoch_available,"
             "memory_initialization_complete,wait_reason,wait_ticks,"
             "network_packets,"
             "network_words,network_word_hops,network_queue_ticks,"
             "receive_dma_transfers,receive_dma_words,receive_dma_active_cycles,"
             "pending_network_receives,pending_receive_dma,"
             "pending_receive_dma_descriptors,pending_completed_receive_frames,"
             "pending_ready_receive_bursts,pending_incoming_frame_assemblies,"
             "bridge_receive_bursts,receive_dma_authorization_available,"
             "bridge_head_source,bridge_head_software_visible,"
             "bridge_head_scheduled,first_unscheduled_payload_offset,"
             "first_unscheduled_payload_source,"
             "first_unscheduled_payload_descriptor_count,"
             "first_unscheduled_payload_descriptor_route,"
             "first_unscheduled_payload_descriptor_iteration,"
             "first_unscheduled_payload_descriptor_remaining_words,"
             "pending_global_dma,"
             "pending_memory,synchronization_events,synchronization_grants\n");
    const char* kind = snapshot.kind == nullptr ? "unknown" : snapshot.kind;
    output << tileId_ << ',' << kind << ',' << snapshot.wallTimeMilliseconds << ','
           << snapshot.simulationTick << ',' << snapshot.instructions << ',' << snapshot.cpuCycles
           << ',' << snapshot.taskFinishEvents << ',' << snapshot.taskFinishEventsAvailable << ','
           << snapshot.physicalGlobalDMASubmitted << ',' << snapshot.physicalGlobalDMACompleted
           << ',' << snapshot.analogCommandsSubmitted << ',' << snapshot.analogCommandsCompleted
           << ',' << snapshot.localEpoch << ',' << snapshot.localEpochAvailable << ','
           << snapshot.memoryInitializationComplete << ',' << snapshot.waitReason << ','
           << snapshot.waitTicks << ',' << snapshot.networkPackets << ',' << snapshot.networkWords
           << ',' << snapshot.networkWordHops << ',' << snapshot.networkQueueTicks << ','
           << snapshot.receiveDMATransfers << ',' << snapshot.receiveDMAWords << ','
           << snapshot.receiveDMAActiveCycles << ',' << snapshot.pendingNetworkReceives << ','
           << snapshot.pendingReceiveDMA << ',' << snapshot.pendingReceiveDMADescriptors << ','
           << snapshot.pendingCompletedReceiveFrames << ',' << snapshot.pendingReadyReceiveBursts
           << ',' << snapshot.pendingIncomingFrameAssemblies << ',' << snapshot.bridgeReceiveBursts
           << ',' << snapshot.receiveDMAAuthorizationAvailable << ',' << snapshot.bridgeHeadSource
           << ',' << snapshot.bridgeHeadSoftwareVisible << ',' << snapshot.bridgeHeadScheduled
           << ',' << snapshot.firstUnscheduledPayloadOffset << ','
           << snapshot.firstUnscheduledPayloadSource << ','
           << snapshot.firstUnscheduledPayloadDescriptorCount << ','
           << snapshot.firstUnscheduledPayloadDescriptorRoute << ','
           << snapshot.firstUnscheduledPayloadDescriptorIteration << ','
           << snapshot.firstUnscheduledPayloadDescriptorRemainingWords << ','
           << snapshot.pendingGlobalDMA << ',' << snapshot.pendingMemory << ','
           << snapshot.synchronizationEvents << ',' << snapshot.synchronizationGrants << '\n';
    ++progressSnapshotCount_;
    if (std::string(kind) == "watchdog")
    {
        ++progressWatchdogCount_;
    }
    ++progressCounterFlushCount_;
    // Flush every bounded diagnostic stream at the same checkpoint.  This
    // keeps a watchdog report useful even when SST terminates the simulation
    // immediately after the snapshot.
    output.flush();
    if (waitStream_.is_open())
        waitStream_.flush();
    if (networkStream_.is_open())
        networkStream_.flush();
    if (receiveDMAStream_.is_open())
        receiveDMAStream_.flush();
    if (analogStream_.is_open())
        analogStream_.flush();
    if (memoryStream_.is_open())
        memoryStream_.flush();
    if (transmitBlockedStream_.is_open())
        transmitBlockedStream_.flush();
}

void PerformanceProfile::writeSummary(const SummarySnapshot& snapshot)
{
    if (!enabled_)
        return;
    MeasurementWriter::writeSummary(tileId_, outputDirectory_, snapshot,
                                    {
                                        traceEnabled_
                                            ? SummaryMetric{clockRegressionCount_}
                                            : SummaryMetric::notMeasured(clockRegressionCount_),
                                        progressSnapshotCount_,
                                        progressWatchdogCount_,
                                        progressCounterFlushCount_,
                                    });
}

std::ofstream& PerformanceProfile::open(std::ofstream& stream, const std::string& suffix,
                                        const char* header)
{
    if (stream.is_open())
    {
        return stream;
    }
    stream.open(path(suffix), std::ios::out | std::ios::trunc);
    if (!stream.is_open())
    {
        throw std::runtime_error("could not open performance profile file '" + path(suffix) + "'");
    }
    stream << header;
    return stream;
}

std::string PerformanceProfile::path(const std::string& suffix) const
{
    return outputDirectory_ + "/tile-" + std::to_string(tileId_) + "-" + suffix;
}

bool GlobalRAMProgressSnapshot::reconciles() const noexcept
{
    if (physicalDMACompleted > physicalDMASubmitted || readinessReleased > readinessBlocked)
    {
        return false;
    }
    if (queuedRequests > UINT64_MAX - activeRequests)
    {
        return false;
    }
    return physicalDMASubmitted - physicalDMACompleted == queuedRequests + activeRequests &&
           readinessBlocked - readinessReleased == readinessCurrentlyBlocked;
}

bool GlobalRAMRequestTimeline::reconciles() const noexcept
{
    return requestSequence != 0 && byteCount != 0 && arrivalCycle <= readinessCycle &&
           readinessCycle <= serviceStartCycle && serviceStartCycle <= completionCycle &&
           completionCycle - serviceStartCycle == serviceCycles;
}

bool GlobalRAMTeardownTimeline::reconciles() const noexcept
{
    return arrivalCycle <= releaseCycle;
}

namespace
{

void accumulateOrThrow(std::uint64_t& total, std::uint64_t value, const char* description)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
    {
        throw std::overflow_error(description);
    }
    total += value;
}

} // namespace

void GlobalRAMPerformanceProfile::configure(const std::string& outputDirectory)
{
    outputDirectory_ = outputDirectory;
    enabled_ = !outputDirectory_.empty();
    if (!enabled_)
    {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(outputDirectory_, error);
    if (error)
    {
        throw std::runtime_error("could not create global RAM profile directory '" +
                                 outputDirectory_ + "': " + error.message());
    }
}

void GlobalRAMPerformanceProfile::recordProgressSnapshot(const GlobalRAMProgressSnapshot& snapshot)
{
    if (!enabled_)
    {
        return;
    }
    if (!snapshot.reconciles())
    {
        throw std::logic_error("global RAM progress counters do not reconcile");
    }
    if (!progressStream_.is_open())
    {
        progressStream_.open(path("progress"), std::ios::out | std::ios::trunc);
        if (!progressStream_.is_open())
        {
            throw std::runtime_error("could not open global RAM progress profile");
        }
        progressStream_ << "kind,wall_time_ms,simulation_cycle,"
                           "physical_dma_submitted,physical_dma_completed,"
                           "physical_dma_bytes_completed,readiness_blocked,"
                           "readiness_released,readiness_currently_blocked,"
                           "readiness_publications,queued_requests,active_requests\n";
    }
    progressStream_ << (snapshot.kind == nullptr ? "unknown" : snapshot.kind) << ','
                    << snapshot.wallTimeMilliseconds << ',' << snapshot.simulationCycle << ','
                    << snapshot.physicalDMASubmitted << ',' << snapshot.physicalDMACompleted << ','
                    << snapshot.physicalDMABytesCompleted << ',' << snapshot.readinessBlocked << ','
                    << snapshot.readinessReleased << ',' << snapshot.readinessCurrentlyBlocked
                    << ',' << snapshot.readinessPublications << ',' << snapshot.queuedRequests
                    << ',' << snapshot.activeRequests << '\n';
    progressStream_.flush();
}

void GlobalRAMPerformanceProfile::recordRequestTimeline(const GlobalRAMRequestTimeline& timeline)
{
    if (!enabled_)
    {
        return;
    }
    if (!timeline.reconciles())
    {
        throw std::logic_error("global RAM request timeline does not reconcile");
    }
    const std::uint64_t readinessDelay = timeline.readinessCycle - timeline.arrivalCycle;
    const std::uint64_t queueDelay = timeline.serviceStartCycle - timeline.readinessCycle;
    accumulateOrThrow(requestCount_, 1, "global RAM request timeline count overflowed");
    accumulateOrThrow(readinessDelayCycles_, readinessDelay,
                      "global RAM readiness-delay timeline overflowed");
    accumulateOrThrow(queueDelayCycles_, queueDelay, "global RAM queue-delay timeline overflowed");
    accumulateOrThrow(serviceCycles_, timeline.serviceCycles,
                      "global RAM service timeline overflowed");
    if (!requestStream_.is_open())
    {
        requestStream_.open(path("requests"), std::ios::out | std::ios::trunc);
        if (!requestStream_.is_open())
        {
            throw std::runtime_error("could not open global RAM request timeline");
        }
        requestStream_ << "request_sequence,tile_id,execution_id,token_id,"
                          "logical_iteration,global_offset,scratchpad_offset,byte_count,"
                          "direction,request_flags,arrival_cycle,readiness_cycle,"
                          "service_start_cycle,completion_cycle,queue_cycles,"
                          "readiness_wait_cycles,service_cycles\n";
    }
    requestStream_ << timeline.requestSequence << ',' << timeline.tileId << ','
                   << timeline.executionId << ',' << timeline.tokenId << ','
                   << timeline.logicalIteration << ',' << timeline.globalOffset << ','
                   << timeline.scratchpadOffset << ',' << timeline.byteCount << ','
                   << (timeline.write ? "write" : "read") << ',' << timeline.requestFlags << ','
                   << timeline.arrivalCycle << ',' << timeline.readinessCycle << ','
                   << timeline.serviceStartCycle << ',' << timeline.completionCycle << ','
                   << queueDelay << ',' << readinessDelay << ',' << timeline.serviceCycles << '\n';
}

void GlobalRAMPerformanceProfile::recordTeardownTimeline(const GlobalRAMTeardownTimeline& timeline)
{
    if (!enabled_)
    {
        return;
    }
    if (!timeline.reconciles())
    {
        throw std::logic_error("global RAM teardown timeline does not reconcile");
    }
    const std::uint64_t waitCycles = timeline.releaseCycle - timeline.arrivalCycle;
    accumulateOrThrow(teardownCount_, 1, "global RAM teardown timeline count overflowed");
    accumulateOrThrow(teardownWaitCycles_, waitCycles,
                      "global RAM teardown-wait timeline overflowed");
    if (!teardownStream_.is_open())
    {
        teardownStream_.open(path("teardowns"), std::ios::out | std::ios::trunc);
        if (!teardownStream_.is_open())
        {
            throw std::runtime_error("could not open global RAM teardown timeline");
        }
        teardownStream_ << "execution_id,tile_id,arrival_cycle,release_cycle,wait_cycles\n";
    }
    teardownStream_ << timeline.executionId << ',' << timeline.tileId << ','
                    << timeline.arrivalCycle << ',' << timeline.releaseCycle << ',' << waitCycles
                    << '\n';
}

bool GlobalRAMPerformanceProfile::requestTotalsReconcile(std::uint64_t requestCount,
                                                         std::uint64_t readinessDelayCycles,
                                                         std::uint64_t queueDelayCycles,
                                                         std::uint64_t serviceCycles) const noexcept
{
    return !enabled_ ||
           (requestCount_ == requestCount && readinessDelayCycles_ == readinessDelayCycles &&
            queueDelayCycles_ == queueDelayCycles && serviceCycles_ == serviceCycles);
}

bool GlobalRAMPerformanceProfile::teardownTotalsReconcile(
    std::uint64_t teardownCount, std::uint64_t teardownWaitCycles) const noexcept
{
    return !enabled_ ||
           (teardownCount_ == teardownCount && teardownWaitCycles_ == teardownWaitCycles);
}

std::string GlobalRAMPerformanceProfile::path(const char* suffix) const
{
    return outputDirectory_ + "/global-ram-" + suffix + ".csv";
}

bool MemoryInitializationBarrierTimeline::reconciles() const noexcept
{
    return arrivalCycle <= releaseCycle;
}

void MemoryInitializationBarrierPerformanceProfile::configure(const std::string& outputDirectory)
{
    outputDirectory_ = outputDirectory;
    enabled_ = !outputDirectory_.empty();
    if (!enabled_)
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(outputDirectory_, error);
    if (error)
    {
        throw std::runtime_error(
            "could not create memory initialization barrier profile directory '" +
            outputDirectory_ + "': " + error.message());
    }
}

void MemoryInitializationBarrierPerformanceProfile::record(
    const MemoryInitializationBarrierTimeline& timeline)
{
    if (!enabled_)
    {
        return;
    }
    if (!timeline.reconciles())
    {
        throw std::logic_error("memory initialization barrier timeline does not reconcile");
    }
    const std::uint64_t waitCycles = timeline.releaseCycle - timeline.arrivalCycle;
    accumulateOrThrow(tileCount_, 1, "memory initialization barrier timeline count overflowed");
    accumulateOrThrow(tileWaitCycles_, waitCycles,
                      "memory initialization barrier wait timeline overflowed");
    firstArrivalCycle_ = std::min(firstArrivalCycle_, timeline.arrivalCycle);
    lastReleaseCycle_ = std::max(lastReleaseCycle_, timeline.releaseCycle);
    if (!stream_.is_open())
    {
        stream_.open(path(), std::ios::out | std::ios::trunc);
        if (!stream_.is_open())
        {
            throw std::runtime_error("could not open memory initialization barrier timeline");
        }
        stream_ << "tile_id,arrival_cycle,release_cycle,wait_cycles\n";
    }
    stream_ << timeline.tileId << ',' << timeline.arrivalCycle << ',' << timeline.releaseCycle
            << ',' << waitCycles << '\n';
}

bool MemoryInitializationBarrierPerformanceProfile::totalsReconcile(
    std::uint64_t tileCount, std::uint64_t tileWaitCycles,
    std::uint64_t barrierWaitCycles) const noexcept
{
    if (!enabled_)
    {
        return true;
    }
    return tileCount_ == tileCount && tileWaitCycles_ == tileWaitCycles &&
           firstArrivalCycle_ != std::numeric_limits<std::uint64_t>::max() &&
           lastReleaseCycle_ >= firstArrivalCycle_ &&
           lastReleaseCycle_ - firstArrivalCycle_ == barrierWaitCycles;
}

std::string MemoryInitializationBarrierPerformanceProfile::path() const
{
    return outputDirectory_ + "/memory-init-barrier.csv";
}

} // namespace Mittens
} // namespace SST
