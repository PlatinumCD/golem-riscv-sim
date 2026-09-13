#include "sst_config.h"
#include "tileMeasurements.h"
#include <sst/core/output.h>

namespace SST::Mittens
{

void reportTileProfile(SST::Output& output, const TileMeasurementSnapshot& data)
{
    output.verbose(
        CALL_INFO, 1, 0,
        "MITTENS_PROFILE tile=%u instructions=%llu vector_instructions=%llu cpu_cycles=%llu "
        "issue_width=%u grants=%llu events=%llu "
        "stop_quantum=%llu stop_nic_tx=%llu stop_nic_rx=%llu "
        "stop_nic_rx_dma_submit=%llu stop_nic_rx_software_claim=%llu "
        "stop_analog_submit=%llu stop_analog_wait=%llu "
        "stop_analog_submit_batch=%llu analog_batch_records=%llu "
        "stop_task_start=%llu stop_task_finish=%llu "
        "stop_memory=%llu stop_memory_init=%llu stop_memory_batch=%llu "
        "memory_batch_records=%llu memory_batch_logical_accesses=%llu "
        "stop_memory_fence=%llu stop_epoch_barrier=%llu "
        "memory_requests=%llu "
        "memory_responses=%llu memory_reads=%llu memory_writes=%llu "
        "memory_init_handshakes=%llu memory_init_accesses=%llu "
        "memory_init_read_bytes=%llu memory_init_write_bytes=%llu "
        "memory_init_cycles=%llu "
        "network_tx_packets=%llu network_tx_words=%llu "
        "network_rx_packets=%llu network_rx_words=%llu "
        "rx_dma_transfers=%llu rx_dma_words=%llu "
        "rx_dma_active_cycles=%llu "
        "analog_active_cycles=%llu analog_link_beats=%llu "
        "analog_set=%llu analog_load=%llu "
        "analog_compute=%llu analog_store=%llu analog_move=%llu "
        "analog_input_words=%llu analog_output_words=%llu\n",
        static_cast<unsigned>(data.configuration.tileId),
        static_cast<unsigned long long>(data.accounting.total.instructions),
        static_cast<unsigned long long>(data.accounting.total.vectors),
        static_cast<unsigned long long>(data.accounting.totalCycles),
        static_cast<unsigned>(data.configuration.cpuIssueWidth),
        static_cast<unsigned long long>(data.cpu.synchronizationGrants_),
        static_cast<unsigned long long>(data.cpu.synchronizationEvents_),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_QUANTUM_END]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_TRANSMIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_SUBMIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_WAIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH]),
        static_cast<unsigned long long>(data.cpu.analogSubmitBatchTransportRecords_),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_START]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_FINISH]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_ACCESS]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_BATCH]),
        static_cast<unsigned long long>(data.cpu.memoryBatchTransportRecords_),
        static_cast<unsigned long long>(data.cpu.memoryBatchLogicalAccesses_),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_FENCE]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE]),
        static_cast<unsigned long long>(data.memory.memoryRequests_),
        static_cast<unsigned long long>(data.memory.memoryResponses_),
        static_cast<unsigned long long>(data.memory.memoryReads_),
        static_cast<unsigned long long>(data.memory.memoryWrites_),
        static_cast<unsigned long long>(data.barriers.memoryInitializationHandshakes_),
        static_cast<unsigned long long>(data.barriers.memoryInitializationAccesses_),
        static_cast<unsigned long long>(data.barriers.memoryInitializationReadBytes_),
        static_cast<unsigned long long>(data.barriers.memoryInitializationWriteBytes_),
        static_cast<unsigned long long>(data.barriers.memoryInitializationCycles_),
        static_cast<unsigned long long>(data.tx.networkTransmitPackets),
        static_cast<unsigned long long>(data.tx.networkTransmitWords),
        static_cast<unsigned long long>(data.rx.networkReceivePackets),
        static_cast<unsigned long long>(data.rx.networkReceiveWords),
        static_cast<unsigned long long>(data.rx.receiveDMATransfers),
        static_cast<unsigned long long>(data.rx.receiveDMAWords),
        static_cast<unsigned long long>(data.rx.receiveDMAActiveCycles),
        static_cast<unsigned long long>(data.analog.elapsed),
        static_cast<unsigned long long>(data.analog.linkBeats),
        static_cast<unsigned long long>(
            data.analog.operations[MITTENS_ANALOG_OPERATION_SET_MATRIX]),
        static_cast<unsigned long long>(
            data.analog.operations[MITTENS_ANALOG_OPERATION_LOAD_VECTOR]),
        static_cast<unsigned long long>(data.analog.operations[MITTENS_ANALOG_OPERATION_COMPUTE]),
        static_cast<unsigned long long>(
            data.analog.operations[MITTENS_ANALOG_OPERATION_STORE_VECTOR]),
        static_cast<unsigned long long>(
            data.analog.operations[MITTENS_ANALOG_OPERATION_MOVE_VECTOR]),
        static_cast<unsigned long long>(data.analog.inputWords),
        static_cast<unsigned long long>(data.analog.outputWords));

    const ScratchpadTimingStatistics scratchpad = data.scratchpad;
    output.verbose(
        CALL_INFO, 1, 0,
        "MITTENS_SCRATCHPAD_PROFILE tile=%u enabled=%u capacity_bytes=%llu "
        "cpu_requests=%llu dma_transfers=%llu dma_bytes=%llu "
        "bank_conflicts=%llu queue_cycles=%llu active_cycles=%llu "
        "service_cycles=%llu "
        "stop_dma_submit=%llu stop_dma_wait=%llu "
        "stop_dma_submit_batch=%llu dma_submit_batch_records=%llu "
        "stop_dma_wait_batch=%llu dma_wait_batch_records=%llu "
        "dma_macro_records=%llu\n",
        static_cast<unsigned>(data.configuration.tileId),
        data.configuration.scratchpadEnabled ? 1U : 0U,
        static_cast<unsigned long long>(data.configuration.scratchpadBytes),
        static_cast<unsigned long long>(scratchpad.cpuRequests),
        static_cast<unsigned long long>(scratchpad.dmaTransfers),
        static_cast<unsigned long long>(scratchpad.dmaBytes),
        static_cast<unsigned long long>(scratchpad.bankConflicts),
        static_cast<unsigned long long>(scratchpad.queueCycles),
        static_cast<unsigned long long>(scratchpad.activeCycles),
        static_cast<unsigned long long>(scratchpad.activeCycles),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT]),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH]),
        static_cast<unsigned long long>(data.cpu.globalDMASubmitBatchTransportRecords_),
        static_cast<unsigned long long>(
            data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH]),
        static_cast<unsigned long long>(data.cpu.globalDMAWaitBatchTransportRecords_),
        static_cast<unsigned long long>(data.cpu.globalDMAMacroRunTransportRecords_));

    output.verbose(
        CALL_INFO, 1, 0,
        "MITTENS_PROFILE_TIMING tile=%u network_word_hops=%llu "
        "network_transit_ticks=%llu network_endpoint_queue_ticks=%llu "
        "wait_nic_tx_ticks=%llu wait_nic_rx_ticks=%llu "
        "wait_rx_dma_submit_ticks=%llu "
        "wait_rx_software_claim_ticks=%llu wait_analog_submit_ticks=%llu "
        "wait_analog_completion_ticks=%llu wait_memory_ticks=%llu "
        "wait_memory_init_ticks=%llu "
        "wait_scratchpad_dma_submit_ticks=%llu "
        "wait_scratchpad_dma_wait_ticks=%llu "
        "wait_scratchpad_dma_wait_batch_ticks=%llu "
        "wait_epoch_barrier_ticks=%llu\n",
        static_cast<unsigned>(data.configuration.tileId),
        static_cast<unsigned long long>(data.tx.networkWordHops),
        static_cast<unsigned long long>(data.rx.networkTransitTicks),
        static_cast<unsigned long long>(data.tx.networkEndpointQueueTicks),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_NIC_TRANSMIT]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            data.cpu.waitTicks_[MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_ANALOG_SUBMIT]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_ANALOG_WAIT]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_MEMORY_ACCESS]),
        static_cast<unsigned long long>(
            data.cpu.waitTicks_[MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE]),
        static_cast<unsigned long long>(
            data.cpu.waitTicks_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT]),
        static_cast<unsigned long long>(data.cpu.waitTicks_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT]),
        static_cast<unsigned long long>(
            data.cpu.waitTicks_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH]),
        static_cast<unsigned long long>(
            data.cpu.waitTicks_[MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE]));
}

void reportTransmitOpportunity(SST::Output& output, const TileMeasurementSnapshot& data)
{
    const auto& scratchpad = data.scratchpad;
    output.output(
        "MITTENS_TX_OPPORTUNITY tile=%u tx_streams=%u "
        "ready_1_cycles=%llu "
        "ready_2_cycles=%llu ready_3_cycles=%llu ready_4plus_cycles=%llu "
        "active_0_cycles=%llu active_1_cycles=%llu active_2_cycles=%llu "
        "active_3_cycles=%llu active_4_cycles=%llu "
        "directions_0_cycles=%llu directions_1_cycles=%llu "
        "directions_2_cycles=%llu directions_3_cycles=%llu "
        "directions_4_cycles=%llu "
        "independent_ready_cycles=%llu same_direction_ready_cycles=%llu "
        "multi_lane_independent_active_cycles=%llu "
        "serialization_stall_cycles=%llu "
        "independent_serialization_stall_cycles=%llu "
        "serialization_delayed_bytes=%llu "
        "serialization_byte_cycles=%llu tx_dma_active_cycles=%llu "
        "queue_occupancy_cycle_sum=%llu queue_observed_cycles=%llu "
        "max_queue_occupancy=%u spm_read_requests=%llu "
        "spm_write_requests=%llu spm_read_bank_conflicts=%llu "
        "spm_write_bank_conflicts=%llu max_spm_reads_same_cycle=%u "
        "max_spm_writes_same_cycle=%u tx_fifo_empty_cycles=%llu "
        "tx_fifo_full_cycles=%llu tx_fifo_empty_lane_cycles=%llu "
        "tx_fifo_full_lane_cycles=%llu\n",
        static_cast<unsigned>(data.configuration.tileId),
        static_cast<unsigned>(data.configuration.transmitDMAStreams),
        static_cast<unsigned long long>(data.tx.transmitReadyCycles[1]),
        static_cast<unsigned long long>(data.tx.transmitReadyCycles[2]),
        static_cast<unsigned long long>(data.tx.transmitReadyCycles[3]),
        static_cast<unsigned long long>(data.tx.transmitReadyCycles[4]),
        static_cast<unsigned long long>(data.tx.transmitActiveLaneCycles[0]),
        static_cast<unsigned long long>(data.tx.transmitActiveLaneCycles[1]),
        static_cast<unsigned long long>(data.tx.transmitActiveLaneCycles[2]),
        static_cast<unsigned long long>(data.tx.transmitActiveLaneCycles[3]),
        static_cast<unsigned long long>(data.tx.transmitActiveLaneCycles[4]),
        static_cast<unsigned long long>(data.tx.transmitReadyDirectionCycles[0]),
        static_cast<unsigned long long>(data.tx.transmitReadyDirectionCycles[1]),
        static_cast<unsigned long long>(data.tx.transmitReadyDirectionCycles[2]),
        static_cast<unsigned long long>(data.tx.transmitReadyDirectionCycles[3]),
        static_cast<unsigned long long>(data.tx.transmitReadyDirectionCycles[4]),
        static_cast<unsigned long long>(data.tx.transmitIndependentReadyCycles),
        static_cast<unsigned long long>(data.tx.transmitSameDirectionReadyCycles),
        static_cast<unsigned long long>(data.tx.transmitMultipleActiveIndependentCycles),
        static_cast<unsigned long long>(data.tx.transmitSerializationStallCycles),
        static_cast<unsigned long long>(data.tx.transmitIndependentSerializationStallCycles),
        static_cast<unsigned long long>(data.tx.transmitSerializationDelayedBytes),
        static_cast<unsigned long long>(data.tx.transmitSerializationByteCycles),
        static_cast<unsigned long long>(data.tx.transmitDMAActiveCycles),
        static_cast<unsigned long long>(data.tx.transmitQueueOccupancyCycleSum),
        static_cast<unsigned long long>(data.tx.transmitOpportunityObservedCycles),
        static_cast<unsigned>(data.tx.transmitMaximumQueueOccupancyObserved),
        static_cast<unsigned long long>(scratchpad.readRequests),
        static_cast<unsigned long long>(scratchpad.writeRequests),
        static_cast<unsigned long long>(scratchpad.readBankConflicts),
        static_cast<unsigned long long>(scratchpad.writeBankConflicts),
        static_cast<unsigned>(scratchpad.maxReadsServicedSameCycle),
        static_cast<unsigned>(scratchpad.maxWritesServicedSameCycle),
        static_cast<unsigned long long>(data.tx.transmitFIFOEmptyCycles),
        static_cast<unsigned long long>(data.tx.transmitFIFOFullCycles),
        static_cast<unsigned long long>(data.tx.transmitFIFOEmptyLaneCycles),
        static_cast<unsigned long long>(data.tx.transmitFIFOFullLaneCycles));
}

SummarySnapshot makeTileSummary(const TileMeasurementSnapshot& data)
{
    SummarySnapshot result;
    result.provenance = data.provenance;
    result.timebase = data.timebase;
    const auto metric = [](bool available, std::uint64_t value)
    { return available ? SummaryMetric{value} : SummaryMetric::disabled(); };
    result.finishTick = metric(true, data.finishTick);
    result.instructions = metric(data.cpuAvailable, data.accounting.total.instructions);
    result.vectorInstructions = metric(data.cpuAvailable, data.accounting.total.vectors);
    result.cpuCycles = metric(data.cpuAvailable, data.accounting.totalCycles);
    result.synchronizationGrants = metric(data.cpuAvailable, data.cpu.synchronizationGrants_);
    result.synchronizationEvents = metric(data.cpuAvailable, data.cpu.synchronizationEvents_);
    result.networkPackets = metric(data.networkAvailable, data.tx.networkTransmitPackets);
    result.networkWords = metric(data.networkAvailable, data.tx.networkTransmitWords);
    result.networkWordHops = metric(data.networkAvailable, data.tx.networkWordHops);
    result.networkTransitTicks = metric(data.networkAvailable, data.rx.networkTransitTicks);
    result.networkQueueTicks = !data.networkAvailable ? SummaryMetric::disabled()
                               : data.configuration.transmitDMAStreams == 1
                                   ? SummaryMetric{data.tx.networkEndpointQueueTicks}
                                   : SummaryMetric::notMeasured(data.tx.networkEndpointQueueTicks);
    result.physicalGlobalDMASubmitted = metric(data.globalDMAAvailable, data.globalDMA.submitted);
    result.physicalGlobalDMACompleted = metric(data.globalDMAAvailable, data.globalDMA.completed);
    result.analogCommandsSubmitted = metric(data.analogAvailable, data.analog.submitted);
    result.analogCommandsCompleted = metric(data.analogAvailable, data.analog.completed);
    result.analogActiveCycles = metric(data.analogAvailable, data.analog.elapsed);
    result.analogLinkBeats = metric(data.analogAvailable, data.analog.linkBeats);
    result.receiveDMAActiveCycles = metric(data.networkAvailable, data.rx.receiveDMAActiveCycles);
    result.transmitDMAActiveCycles =
        metric(data.networkAvailable && data.scratchpadAvailable, data.tx.transmitDMAActiveCycles);
    result.scratchpadServiceCycles = metric(data.scratchpadAvailable, data.scratchpad.activeCycles);
    result.scratchpadReadServiceCycles =
        metric(data.scratchpadAvailable, data.scratchpad.readServiceCycles);
    result.scratchpadWriteServiceCycles =
        metric(data.scratchpadAvailable, data.scratchpad.writeServiceCycles);
    result.scratchpadBankConflicts =
        metric(data.scratchpadAvailable, data.scratchpad.bankConflicts);
    result.scratchpadQueueCycles = metric(data.scratchpadAvailable, data.scratchpad.queueCycles);
    result.scratchpadCPURequests = metric(data.scratchpadAvailable, data.scratchpad.cpuRequests);
    result.scratchpadDMATransfers = metric(data.scratchpadAvailable, data.scratchpad.dmaTransfers);
    result.scratchpadDMABytes = metric(data.scratchpadAvailable, data.scratchpad.dmaBytes);
    result.transmitBlockedTicks = !data.networkAvailable ? SummaryMetric::disabled()
                                  : data.configuration.transmitDMAStreams == 1
                                      ? SummaryMetric{data.tx.transmitBlockedTicks}
                                      : SummaryMetric::notMeasured(data.tx.transmitBlockedTicks);
    result.transmitBlockedEvents = !data.networkAvailable ? SummaryMetric::disabled()
                                   : data.configuration.transmitDMAStreams == 1
                                       ? SummaryMetric{data.tx.transmitBlockedEvents}
                                       : SummaryMetric::notMeasured(data.tx.transmitBlockedEvents);
    result.transmitBlockedRetries =
        !data.networkAvailable ? SummaryMetric::disabled()
        : data.configuration.transmitDMAStreams == 1
            ? SummaryMetric{data.tx.transmitBlockedRetries}
            : SummaryMetric::notMeasured(data.tx.transmitBlockedRetries);
    result.transmitMaximumQueueOccupancy =
        !data.networkAvailable ? SummaryMetric::disabled()
        : data.configuration.transmitDMAStreams == 1
            ? SummaryMetric{data.tx.transmitMaximumQueueOccupancy}
            : SummaryMetric::notMeasured(data.tx.transmitMaximumQueueOccupancy);
    result.maximumOutstandingMemoryRequests =
        metric(data.memoryAvailable, data.memory.maximumOutstandingMemoryRequests_);
    result.maximumOutstandingMemoryReads =
        metric(data.memoryAvailable, data.memory.maximumOutstandingMemoryReads_);
    result.maximumStoreBufferOccupancy =
        metric(data.memoryAvailable, data.memory.maximumStoreBufferOccupancy_);
    result.storeBufferFullEvents =
        metric(data.memoryAvailable, data.memory.memoryStoreBufferFullEvents_);
    result.vectorMemoryRequestGroups =
        metric(data.memoryAvailable, data.memory.vectorMemoryRequestGroups_);
    result.vectorMemoryGroupRequests =
        metric(data.memoryAvailable, data.memory.vectorMemoryGroupRequests_);
    result.scalarMemoryRequestGroups =
        metric(data.memoryAvailable, data.memory.scalarMemoryRequestGroups_);
    result.scalarMemoryGroupRequests =
        metric(data.memoryAvailable, data.memory.scalarMemoryGroupRequests_);
    for (std::size_t index = 0; index < data.cpu.synchronizationStopCounts_.size(); ++index)
    {
        const auto reason = syncStopReasonName(static_cast<std::uint32_t>(index));
        result.stopCounts.push_back(
            {reason, metric(data.cpuAvailable, data.cpu.synchronizationStopCounts_[index])});
        result.waitTicks.push_back({reason, metric(data.cpuAvailable, data.cpu.waitTicks_[index])});
    }
    if (data.scratchpadAvailable &&
        (data.scratchpad.readServiceCycles > data.scratchpad.activeCycles ||
         data.scratchpad.writeServiceCycles !=
             data.scratchpad.activeCycles - data.scratchpad.readServiceCycles))
        throw std::logic_error("SPM service counters do not reconcile");
    auto& tx = result.transmitObservations;
    tx.observedCycles = metric(data.networkAvailable, data.tx.transmitOpportunityObservedCycles);
    if (data.networkAvailable)
    {
        tx.readyCycles = data.tx.transmitReadyCycles;
        tx.activeCycles = data.tx.transmitActiveLaneCycles;
        tx.directionCycles = data.tx.transmitReadyDirectionCycles;
    }
    return result;
}

void writeTileSummary(PerformanceProfile& profile, SST::Output& output,
                      const TileMeasurementSnapshot& data)
{
    profile.writeSummary(makeTileSummary(data));
    output.output("MITTENS_ANALOG_ACTIVITY tile=%u submitted=%llu completed=%llu "
                  "maximum_outstanding=%llu\n",
                  static_cast<unsigned>(data.configuration.tileId),
                  static_cast<unsigned long long>(data.analog.submitted),
                  static_cast<unsigned long long>(data.analog.completed),
                  static_cast<unsigned long long>(data.analog.maximumOutstanding));
}

} // namespace SST::Mittens
