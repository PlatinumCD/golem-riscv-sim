#include "performanceProfile.h"

#include <filesystem>
#include <stdexcept>

namespace SST {
namespace Mittens {

void PerformanceProfile::configure(
    std::uint32_t tileId,
    const std::string& outputDirectory,
    bool traceEnabled)
{
    tileId_ = tileId;
    outputDirectory_ = outputDirectory;
    enabled_ = !outputDirectory_.empty();
    traceEnabled_ = enabled_ && traceEnabled;
    if (!enabled_) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(outputDirectory_, error);
    if (error) {
        throw std::runtime_error(
            "could not create performance profile directory '" +
            outputDirectory_ + "': " + error.message());
    }
}

void PerformanceProfile::recordWait(
    std::uint64_t startTick,
    std::uint64_t finishTick,
    const char* reason,
    std::uint64_t eventSequence)
{
    if (!traceEnabled_) {
        return;
    }
    if (finishTick < startTick) {
        throw std::logic_error("performance profile wait time went backwards");
    }
    std::ofstream& output = open(
        waitStream_,
        "waits.csv",
        "tile_id,event_sequence,reason,start_tick,finish_tick,duration_ticks\n");
    output << tileId_ << ','
           << eventSequence << ','
           << reason << ','
           << startTick << ','
           << finishTick << ','
           << (finishTick - startTick) << '\n';
}

void PerformanceProfile::recordNetwork(
    const char* event,
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
    std::uint64_t eventTick)
{
    if (!traceEnabled_) {
        return;
    }
    std::ofstream& output = open(
        networkStream_,
        "network.csv",
        "tile_id,event,packet_id,source,destination,route_id,execution_id,"
        "kind,words,protocol_words,payload_words,hops,word_hops,ready_tick,"
        "injection_tick,event_tick,endpoint_queue_ticks,transit_ticks\n");
    const std::uint64_t queueTicks =
        injectionTick >= readyTick ? injectionTick - readyTick : 0;
    const std::uint64_t transitTicks =
        eventTick >= injectionTick ? eventTick - injectionTick : 0;
    output << tileId_ << ','
           << event << ','
           << packetId << ','
           << source << ','
           << destination << ','
           << routeId << ','
           << executionId << ','
           << kind << ','
           << words << ','
           << protocolWords << ','
           << payloadWords << ','
           << hops << ','
           << static_cast<std::uint64_t>(words) * hops << ','
           << readyTick << ','
           << injectionTick << ','
           << eventTick << ','
           << queueTicks << ','
           << transitTicks << '\n';
}

void PerformanceProfile::recordReceiveDMA(
    const char* event,
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
    std::uint32_t invalidationLines)
{
    if (!traceEnabled_) {
        return;
    }
    std::ofstream& output = open(
        receiveDMAStream_,
        "receive-dma.csv",
        "tile_id,event,source,route_id,execution_id,burst_index,words,"
        "schedule_tick,dma_start_cycle,dma_completion_cycle,event_tick,"
        "service_cycles,invalidation_lines\n");
    output << tileId_ << ','
           << event << ','
           << source << ','
           << routeId << ','
           << executionId << ','
           << burstIndex << ','
           << words << ','
           << scheduleTick << ','
           << startCycle << ','
           << completionCycle << ','
           << eventTick << ','
           << serviceCycles << ','
           << invalidationLines << '\n';
}

void PerformanceProfile::recordAnalog(
    const char* phase,
    std::uint64_t ticket,
    std::uint32_t operation,
    std::uint32_t arrayId,
    std::uint64_t deviceCycle,
    std::uint64_t eventTick)
{
    if (!traceEnabled_) {
        return;
    }
    std::ofstream& output = open(
        analogStream_,
        "analog.csv",
        "tile_id,ticket,operation,array_id,phase,device_cycle,event_tick\n");
    output << tileId_ << ','
           << ticket << ','
           << operation << ','
           << arrayId << ','
           << phase << ','
           << deviceCycle << ','
           << eventTick << '\n';
}

void PerformanceProfile::recordMemory(
    const char* event,
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
    std::uint64_t eventTick)
{
    if (!traceEnabled_) {
        return;
    }
    std::ofstream& output = open(
        memoryStream_,
        "memory.csv",
        "tile_id,event,request_id,address,timing_address,guest_pc,guest_ra,"
        "size,direction,"
        "task_id,execution_id,phase,issue_tick,event_tick,latency_ticks\n");
    output << tileId_ << ','
           << event << ','
           << requestId << ','
           << guestAddress << ','
           << timingAddress << ','
           << programCounter << ','
           << returnAddress << ','
           << size << ','
           << (write ? "write" : "read") << ','
           << taskId << ','
           << executionId << ','
           << phase << ','
           << issueTick << ','
           << eventTick << ','
           << (eventTick >= issueTick ? eventTick - issueTick : 0)
           << '\n';
}

void PerformanceProfile::recordTransmitBlocked(
    std::uint64_t eventSequence,
    std::uint32_t routeId,
    std::uint64_t executionId,
    std::uint32_t destination,
    const char* kind,
    std::uint32_t words,
    std::uint64_t startTick,
    std::uint64_t finishTick,
    std::uint64_t retryCount,
    std::uint32_t maximumQueueOccupancy)
{
    if (!traceEnabled_) {
        return;
    }
    if (finishTick < startTick) {
        throw std::logic_error(
            "performance profile transmit-block time went backwards");
    }
    std::ofstream& output = open(
        transmitBlockedStream_,
        "transmit-blocked.csv",
        "tile_id,event_sequence,route_id,execution_id,destination,kind,"
        "words,start_tick,finish_tick,duration_ticks,retry_count,"
        "maximum_queue_occupancy\n");
    output << tileId_ << ','
           << eventSequence << ','
           << routeId << ','
           << executionId << ','
           << destination << ','
           << kind << ','
           << words << ','
           << startTick << ','
           << finishTick << ','
           << (finishTick - startTick) << ','
           << retryCount << ','
           << maximumQueueOccupancy << '\n';
}

void PerformanceProfile::writeSummary(
    std::uint64_t finishTick,
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
    const std::uint64_t* waitTicks,
    const char* const* waitReasonNames,
    std::size_t waitTickCount)
{
    if (!enabled_) {
        return;
    }
    std::ofstream output(path("summary.csv"), std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error(
            "could not open performance profile summary");
    }
    output << "metric,value\n"
           << "tile_id," << tileId_ << '\n'
           << "finish_tick," << finishTick << '\n'
           << "instructions," << instructions << '\n'
           << "vector_instructions," << vectorInstructions << '\n'
           << "cpu_cycles," << cpuCycles << '\n'
           << "network_packets," << networkPackets << '\n'
           << "network_words," << networkWords << '\n'
           << "network_word_hops," << networkWordHops << '\n'
           << "network_transit_ticks," << networkTransitTicks << '\n'
           << "network_endpoint_queue_ticks," << networkQueueTicks << '\n'
           << "analog_active_cycles," << analogActiveCycles << '\n'
           << "analog_link_beats," << analogLinkBeats << '\n'
           << "receive_dma_active_cycles," << receiveDMAActiveCycles << '\n'
           << "transmit_blocked_ticks," << transmitBlockedTicks << '\n'
           << "transmit_blocked_events," << transmitBlockedEvents << '\n'
           << "transmit_blocked_retries," << transmitBlockedRetries << '\n'
           << "transmit_maximum_queue_occupancy,"
           << transmitMaximumQueueOccupancy << '\n';
    for (std::size_t index = 0; index < waitTickCount; ++index) {
        output << "wait_" << waitReasonNames[index] << "_ticks,"
               << waitTicks[index] << '\n';
    }
}

std::ofstream& PerformanceProfile::open(
    std::ofstream& stream,
    const std::string& suffix,
    const char* header)
{
    if (stream.is_open()) {
        return stream;
    }
    stream.open(path(suffix), std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error(
            "could not open performance profile file '" +
            path(suffix) + "'");
    }
    stream << header;
    return stream;
}

std::string PerformanceProfile::path(const std::string& suffix) const
{
    return outputDirectory_ + "/tile-" + std::to_string(tileId_) +
           "-" + suffix;
}

} // namespace Mittens
} // namespace SST
