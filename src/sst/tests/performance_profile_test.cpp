#include "../profiling/performanceProfile.h"
#include "../memory/scratchpad/scratchpadTimingModel.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <stdlib.h>

using SST::Mittens::PerformanceProfile;
using SST::Mittens::GlobalRAMPerformanceProfile;
using SST::Mittens::GlobalRAMProgressSnapshot;
using SST::Mittens::GlobalRAMRequestTimeline;
using SST::Mittens::GlobalRAMTeardownTimeline;
using SST::Mittens::MemoryInitializationBarrierPerformanceProfile;
using SST::Mittens::MemoryInitializationBarrierTimeline;

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    assert(input.is_open());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    char directoryTemplate[] =
        "/tmp/mittens-performance-profile-XXXXXX";
    char* const rawDirectory = mkdtemp(directoryTemplate);
    assert(rawDirectory != nullptr);
    const std::filesystem::path directory(rawDirectory);

    {
        PerformanceProfile profile;
        profile.configure(3, directory.string(), true);
        assert(profile.enabled());
        assert(profile.traceEnabled());

        profile.recordWait(100, 125, "nic-receive-wait", 7);
        profile.recordScratchpadBeat({1, false, 256, 32, 0, 0, 10, 12, 13});
        profile.recordReceiveBoundary("frame-ready", 0, 4, 2, 7, 16, 130000);
        profile.recordWait(150, 125, "shutdown-wait", 8);
        profile.recordNetwork(
            "arrive",
            9,
            0,
            3,
            4,
            2,
            7,
            "frame-payload",
            16,
            0,
            16,
            3,
            80,
            90,
            130);
        profile.recordReceiveDMA(
            "complete",
            0,
            4,
            2,
            7,
            11,
            16,
            130,
            140,
            150,
            150,
            10);
        profile.recordAnalog(
            "compute-start", 12, 3, 1, 40, 160);
        profile.recordMemory(
            "response", 13, 0x80001000, 0x1000, 0x80000200, 0x80000400,
            8, false,
            5, 2, "task", 170, 190);
        profile.recordTransmitBlocked(
            14, 4, 2, 5, "frame-payload", 16, 180, 195, 3, 5);
        profile.recordTransmitBlocked(
            15, 6, 3, 7, "frame-payload", 16, 220, 200, 1, 2);
        SST::Mittens::ProgressSnapshot progress;
        progress.kind = "watchdog";
        progress.wallTimeMilliseconds = 123;
        progress.simulationTick = 456;
        progress.instructions = 789;
        progress.cpuCycles = 790;
        progress.taskFinishEvents = 12;
        progress.taskFinishEventsAvailable = 1;
        progress.physicalGlobalDMASubmitted = 13;
        progress.physicalGlobalDMACompleted = 11;
        progress.analogCommandsSubmitted = 17;
        progress.analogCommandsCompleted = 16;
        progress.localEpoch = 2;
        progress.localEpochAvailable = 1;
        progress.memoryInitializationComplete = 1;
        progress.waitReason = 3;
        progress.waitTicks = 99;
        progress.networkPackets = 4;
        progress.networkWords = 5;
        progress.networkWordHops = 6;
        progress.networkQueueTicks = 7;
        progress.receiveDMATransfers = 8;
        progress.receiveDMAWords = 9;
        progress.receiveDMAActiveCycles = 10;
        progress.pendingNetworkReceives = 11;
        progress.pendingReceiveDMA = 12;
        progress.pendingGlobalDMA = 13;
        progress.pendingMemory = 14;
        progress.synchronizationEvents = 15;
        progress.synchronizationGrants = 16;
        SST::Mittens::ProgressSnapshot corruptProgress = progress;
        corruptProgress.physicalGlobalDMACompleted = 14;
        bool corruptProgressRejected = false;
        try {
            profile.recordProgressSnapshot(corruptProgress);
        } catch (const std::logic_error&) {
            corruptProgressRejected = true;
        }
        assert(corruptProgressRejected);
        corruptProgress = progress;
        corruptProgress.localEpochAvailable = 0;
        bool corruptAvailabilityRejected = false;
        try {
            profile.recordProgressSnapshot(corruptProgress);
        } catch (const std::logic_error&) {
            corruptAvailabilityRejected = true;
        }
        assert(corruptAvailabilityRejected);
        corruptProgress = progress;
        corruptProgress.taskFinishEventsAvailable = 0;
        corruptAvailabilityRejected = false;
        try {
            profile.recordProgressSnapshot(corruptProgress);
        } catch (const std::logic_error&) {
            corruptAvailabilityRejected = true;
        }
        assert(corruptAvailabilityRejected);
        profile.recordProgressSnapshot(progress);

        SST::Mittens::SummarySnapshot summary;
        summary.finishTick = 200;
        summary.instructions = 1000;
        summary.vectorInstructions = 20;
        summary.cpuCycles = 500;
        summary.synchronizationGrants = 77;
        summary.synchronizationEvents = 88;
        summary.networkPackets = 2;
        summary.networkWords = 21;
        summary.networkWordHops = 63;
        summary.networkTransitTicks = 40;
        summary.networkQueueTicks = 10;
        summary.physicalGlobalDMASubmitted = 13;
        summary.physicalGlobalDMACompleted = 11;
        summary.analogCommandsSubmitted = 17;
        summary.analogCommandsCompleted = 16;
        summary.analogActiveCycles = 30;
        summary.analogLinkBeats = 12;
        summary.receiveDMAActiveCycles = 13;
        summary.transmitDMAActiveCycles = 10;
        summary.scratchpadServiceCycles = 14;
        summary.scratchpadReadServiceCycles = 15;
        summary.scratchpadWriteServiceCycles = 16;
        summary.scratchpadBankConflicts = 17;
        summary.scratchpadQueueCycles = 18;
        summary.scratchpadCPURequests = 19;
        summary.scratchpadDMATransfers = 20;
        summary.scratchpadDMABytes = 21;
        summary.transmitBlockedTicks = 15;
        summary.transmitBlockedEvents = 1;
        summary.transmitBlockedRetries = 3;
        summary.transmitMaximumQueueOccupancy = 5;
        summary.maximumOutstandingMemoryRequests = 8;
        summary.maximumOutstandingMemoryReads = 4;
        summary.maximumStoreBufferOccupancy = 7;
        summary.storeBufferFullEvents = 2;
        summary.vectorMemoryRequestGroups = 3;
        summary.vectorMemoryGroupRequests = 6;
        summary.scalarMemoryRequestGroups = 9;
        summary.scalarMemoryGroupRequests = 18;
        summary.stopCounts = {
            {"none", 0},
            {"quantum-end", 4},
            {"memory-batch", 5},
            {"memory-fence", 6},
        };
        summary.waitTicks = {
            {"none", 0},
            {"quantum-end", 1},
            {"nic-transmit", 2},
            {"nic-receive-wait", 3},
        };
        profile.writeSummary(summary);
    }

    {
        GlobalRAMPerformanceProfile profile;
        profile.configure(directory.string());
        assert(profile.enabled());

        GlobalRAMProgressSnapshot progress;
        progress.kind = "periodic";
        progress.wallTimeMilliseconds = 321;
        progress.simulationCycle = 654;
        progress.physicalDMASubmitted = 9;
        progress.physicalDMACompleted = 4;
        progress.physicalDMABytesCompleted = 4096;
        progress.readinessBlocked = 6;
        progress.readinessReleased = 2;
        progress.readinessCurrentlyBlocked = 4;
        progress.readinessPublications = 3;
        progress.queuedRequests = 3;
        progress.activeRequests = 2;
        assert(progress.reconciles());
        profile.recordProgressSnapshot(progress);

        GlobalRAMProgressSnapshot corrupt = progress;
        corrupt.physicalDMACompleted = 5;
        bool rejected = false;
        try {
            profile.recordProgressSnapshot(corrupt);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);

        GlobalRAMRequestTimeline request;
        request.requestSequence = 4;
        request.tileId = 1;
        request.executionId = 9;
        request.tokenId = 7;
        request.logicalIteration = 11;
        request.globalOffset = 4096;
        request.scratchpadOffset = 8192;
        request.byteCount = 4096;
        request.requestFlags = 3;
        request.write = true;
        request.arrivalCycle = 100;
        request.readinessCycle = 120;
        request.serviceStartCycle = 130;
        request.completionCycle = 280;
        request.serviceCycles = 150;
        assert(request.reconciles());
        profile.recordRequestTimeline(request);
        assert(profile.requestTotalsReconcile(1, 20, 10, 150));
        assert(!profile.requestTotalsReconcile(1, 20, 30, 150));

        GlobalRAMRequestTimeline corruptRequest = request;
        corruptRequest.readinessCycle = 140;
        bool corruptRequestRejected = false;
        try {
            profile.recordRequestTimeline(corruptRequest);
        } catch (const std::logic_error&) {
            corruptRequestRejected = true;
        }
        assert(corruptRequestRejected);

        GlobalRAMTeardownTimeline teardown;
        teardown.executionId = 9;
        teardown.tileId = 1;
        teardown.arrivalCycle = 300;
        teardown.releaseCycle = 330;
        assert(teardown.reconciles());
        profile.recordTeardownTimeline(teardown);
        assert(profile.teardownTotalsReconcile(1, 30));
        assert(!profile.teardownTotalsReconcile(1, 29));

        GlobalRAMTeardownTimeline corruptTeardown = teardown;
        corruptTeardown.releaseCycle = 299;
        bool corruptTeardownRejected = false;
        try {
            profile.recordTeardownTimeline(corruptTeardown);
        } catch (const std::logic_error&) {
            corruptTeardownRejected = true;
        }
        assert(corruptTeardownRejected);

        progress.kind = "final";
        progress.physicalDMACompleted = 9;
        progress.physicalDMABytesCompleted = 9216;
        progress.readinessReleased = 6;
        progress.readinessCurrentlyBlocked = 0;
        progress.queuedRequests = 0;
        progress.activeRequests = 0;
        assert(progress.reconciles());
        profile.recordProgressSnapshot(progress);
    }

    {
        MemoryInitializationBarrierPerformanceProfile profile;
        profile.configure(directory.string());
        assert(profile.enabled());
        profile.record(MemoryInitializationBarrierTimeline{0, 10, 40});
        profile.record(MemoryInitializationBarrierTimeline{2, 20, 41});
        profile.record(MemoryInitializationBarrierTimeline{3, 30, 42});
        assert(profile.totalsReconcile(3, 63, 32));
        assert(!profile.totalsReconcile(3, 64, 32));

        bool corruptTimelineRejected = false;
        try {
            profile.record(MemoryInitializationBarrierTimeline{1, 50, 49});
        } catch (const std::logic_error&) {
            corruptTimelineRejected = true;
        }
        assert(corruptTimelineRejected);
    }

    const std::string waitText =
        readFile(directory / "tile-3-waits.csv");
    assert(waitText.find("nic-receive-wait,100,125,25") !=
           std::string::npos);
    assert(waitText.find("shutdown-wait,150,150,0") !=
           std::string::npos);

    const std::string networkText =
        readFile(directory / "tile-3-network.csv");
    assert(readFile(directory / "tile-3-scratchpad-beats.csv").find(
        "3,1,read,256,32,0,0,10,12,13") != std::string::npos);
    assert(readFile(directory / "tile-3-receive-boundaries.csv").find(
        "3,frame-ready,0,4,2,7,16,130000") != std::string::npos);
    assert(networkText.find(
               "frame-payload,16,0,16,3,48,80,90,130,10,40") !=
           std::string::npos);

    const std::string summaryText =
        readFile(directory / "tile-3-summary.csv");
    const std::string progressText =
        readFile(directory / "tile-3-progress.csv");
    assert(progressText.find(
               "3,watchdog,123,456,789,790,12,1,13,11,17,16,2,1,1,3,99") !=
           std::string::npos);
    assert(summaryText.find("network_word_hops,63") !=
           std::string::npos);
    assert(summaryText.find("synchronization_grants,77") !=
           std::string::npos);
    assert(summaryText.find("synchronization_events,88") !=
           std::string::npos);
    assert(summaryText.find("physical_global_dma_submitted,13") !=
           std::string::npos);
    assert(summaryText.find("physical_global_dma_completed,11") !=
           std::string::npos);
    assert(summaryText.find("analog_commands_submitted,17") !=
           std::string::npos);
    assert(summaryText.find("analog_commands_completed,16") !=
           std::string::npos);
    assert(summaryText.find("scratchpad_service_cycles,14") !=
           std::string::npos);
    assert(summaryText.find("stop_memory_batch,5") !=
           std::string::npos);
    assert(summaryText.find("stop_memory_fence,6") !=
           std::string::npos);
    assert(summaryText.find("wait_nic-receive-wait_ticks,3") !=
           std::string::npos);
    assert(summaryText.find("transmit_blocked_ticks,15") !=
           std::string::npos);
    assert(summaryText.find("memory_maximum_outstanding_requests,8") !=
           std::string::npos);
    assert(summaryText.find("memory_vector_group_requests,6") !=
           std::string::npos);
    assert(summaryText.find("memory_scalar_request_groups,9") !=
           std::string::npos);
    assert(summaryText.find("memory_scalar_group_requests,18") !=
           std::string::npos);
    assert(summaryText.find("profile_clock_regressions,2") !=
           std::string::npos);
    assert(summaryText.find("progress_snapshots,1") !=
           std::string::npos);
    assert(summaryText.find("progress_watchdog_events,1") !=
           std::string::npos);
    assert(summaryText.find("progress_counter_flushes,1") !=
           std::string::npos);

    const std::string transmitText =
        readFile(directory / "tile-3-transmit-blocked.csv");
    assert(transmitText.find(
               "3,14,4,2,5,frame-payload,16,180,195,15,3,5") !=
           std::string::npos);
    assert(transmitText.find(
               "3,15,6,3,7,frame-payload,16,220,220,0,1,2") !=
           std::string::npos);

    const std::string globalRAMProgressText =
        readFile(directory / "global-ram-progress.csv");
    assert(globalRAMProgressText.find(
               "periodic,321,654,9,4,4096,6,2,4,3,3,2") !=
           std::string::npos);
    assert(globalRAMProgressText.find(
               "final,321,654,9,9,9216,6,6,0,3,0,0") !=
           std::string::npos);
    const std::string globalRAMRequestText =
        readFile(directory / "global-ram-requests.csv");
    assert(globalRAMRequestText.find(
               "4,1,9,7,11,4096,8192,4096,write,3,100,120,130,280,10,20,150") !=
           std::string::npos);
    const std::string globalRAMTeardownText =
        readFile(directory / "global-ram-teardowns.csv");
    assert(globalRAMTeardownText.find("9,1,300,330,30") !=
           std::string::npos);
    const std::string initializationBarrierText =
        readFile(directory / "memory-init-barrier.csv");
    assert(initializationBarrierText.find("0,10,40,30") !=
           std::string::npos);
    assert(initializationBarrierText.find("2,20,41,21") !=
           std::string::npos);
    assert(initializationBarrierText.find("3,30,42,12") !=
           std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    assert(!error);
    return 0;
}
