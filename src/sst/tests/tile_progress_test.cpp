#include "../profiling/tileProgress.h"
#include <cassert>

using namespace SST::Mittens;

int main(int, char**)
{
    TileMeasurementSnapshot data;
    data.configuration.epochBarrierEpochs = 4;
    data.finishTick = 100;
    data.accounting.total.instructions = 123;
    data.accounting.totalCycles = 80;
    data.cpu.taskFinishEvents_ = 7;
    data.cpu.synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_START] = 1;
    data.cpu.activeWaitStartTick_ = 90;
    data.cpu.activeWaitReason_ = MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT;
    data.cpu.waitTicks_[MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT] = 20;
    data.cpu.synchronizationEvents_ = 8;
    data.cpu.synchronizationGrants_ = 9;
    data.globalDMA = {10, 8, 2};
    data.analog.submitted = 12;
    data.analog.completed = 11;
    data.barriers.expectedEpochBarrier_ = 2;
    data.barriers.memoryInitializationPhase_ = false;
    data.memory.outstandingMemoryWrites_ = 3;
    data.memory.outstandingMemoryReads_ = 4;
    data.tx.networkTransmitPackets = 11;
    data.rx.networkReceivePackets = 12;
    data.tx.networkTransmitWords = 13;
    data.rx.networkReceiveWords = 14;
    data.tx.networkWordHops = 15;
    data.tx.networkEndpointQueueTicks = 16;
    data.rx.receiveDMATransfers = 17;
    data.rx.receiveDMAWords = 18;
    data.rx.receiveDMAActiveCycles = 19;
    RxStatus rx;
    rx.pendingNetwork = 20;
    rx.pendingTransfers = 21;
    rx.pendingDescriptors = 22;
    rx.completedFrames = 23;
    rx.readyBursts = 24;
    rx.incomingFrames = 25;
    rx.bridgeReceiveBursts = 26;
    rx.authorizationAvailable = true;
    rx.bridgeHead = ReceiveBurstInfo{0, 27, 32, true};
    rx.bridgeHeadScheduled = true;
    rx.firstUnscheduledPayloadOffset = 28;
    rx.firstUnscheduledPayloadSource = 29;
    rx.firstUnscheduledPayloadDescriptorCount = 30;
    rx.firstUnscheduledPayloadDescriptorRoute = 31;
    rx.firstUnscheduledPayloadDescriptorIteration = 32;
    rx.firstUnscheduledPayloadDescriptorRemainingWords = 33;
    const auto p = makeTileProgressSnapshot(data, rx, {3}, 5, 200, "watchdog");
    assert(p.wallTimeMilliseconds == 200 && p.simulationTick == 100);
    assert(p.instructions == 123 && p.cpuCycles == 80 && p.taskFinishEvents == 7);
    assert(p.taskFinishEventsAvailable == 1 && p.localEpochAvailable == 1 && p.localEpoch == 2);
    assert(p.memoryInitializationComplete == 1 && p.waitTicks == 30);
    assert(p.physicalGlobalDMASubmitted == 10 && p.physicalGlobalDMACompleted == 8);
    assert(p.analogCommandsSubmitted == 12 && p.analogCommandsCompleted == 11);
    assert(p.networkPackets == 23 && p.networkWords == 27);
    assert(p.networkWordHops == 15 && p.networkQueueTicks == 16);
    assert(p.receiveDMATransfers == 17 && p.receiveDMAWords == 18 && p.receiveDMAActiveCycles == 19);
    assert(p.pendingNetworkReceives == 23 && p.pendingReceiveDMA == 21);
    assert(p.pendingReceiveDMADescriptors == 22 && p.pendingCompletedReceiveFrames == 23);
    assert(p.pendingReadyReceiveBursts == 24 && p.pendingIncomingFrameAssemblies == 25);
    assert(p.bridgeReceiveBursts == 26 && p.receiveDMAAuthorizationAvailable == 1);
    assert(p.bridgeHeadSource == 27 && p.bridgeHeadSoftwareVisible == 1 && p.bridgeHeadScheduled == 1);
    assert(p.firstUnscheduledPayloadOffset == 28 && p.firstUnscheduledPayloadSource == 29);
    assert(p.firstUnscheduledPayloadDescriptorCount == 30 && p.firstUnscheduledPayloadDescriptorRoute == 31);
    assert(p.firstUnscheduledPayloadDescriptorIteration == 32 && p.firstUnscheduledPayloadDescriptorRemainingWords == 33);
    assert(p.pendingGlobalDMA == 2 && p.pendingMemory == 12);
    assert(p.synchronizationEvents == 8 && p.synchronizationGrants == 9);
    const auto expected = std::string("MITTENS_PROGRESS tile=3 kind=watchdog wall_ms=200 sim_tick=100 ") +
        "instructions=123 task_finishes=7 task_finishes_available=1 "
        "physical_global_dma_submitted=10 physical_global_dma_completed=8 "
        "analog_commands_submitted=12 analog_commands_completed=11 local_epoch=2 "
        "local_epoch_available=1 memory_initialization_complete=1 wait_reason=" +
        std::to_string(MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT) +
        " wait_ticks=30 pending_network=23 pending_rx_dma=21 pending_global_dma=2 pending_memory=12\n";
    assert(formatTileProgress(3, p) == expected);
    assert(formatProgressWatchdog(3, 4, 5, 6, 7) ==
        "MITTENS_PROGRESS_WATCHDOG tile=3 timeout_ms=4 progress_elapsed_ms=5 "
        "deployment_epoch=6 initialization_execution_epoch=7\n");

    data.configuration.epochBarrierEpochs = 0;
    data.cpu.synchronizationStopCounts_.fill(0);
    data.cpu.activeWaitStartTick_.reset();
    const auto idle = makeTileProgressSnapshot(data, {}, {}, 0, 0, nullptr);
    assert(idle.localEpoch == UINT32_MAX && idle.localEpochAvailable == 0);
    assert(idle.taskFinishEventsAvailable == 0 && idle.waitReason == MITTENS_SYNC_STOP_NONE);
    assert(idle.waitTicks == 0 && idle.bridgeHeadSource == UINT32_MAX);
    assert(idle.firstUnscheduledPayloadDescriptorIteration == UINT64_MAX);
    assert(formatTileProgress(0, idle).find("kind=unknown") != std::string::npos);
    data.cpu.activeWaitStartTick_ = 200; // do not subtract a future timestamp
    assert(makeTileProgressSnapshot(data, {}, {}, 0, 0, "final").waitTicks == 20);
    data.cpu.activeWaitReason_ = UINT32_MAX; // unavailable reason is not an array index
    assert(makeTileProgressSnapshot(data, {}, {}, 0, 0, "final").waitTicks == 0);
}
