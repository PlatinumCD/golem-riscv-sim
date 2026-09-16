#include "../analog/analogController.h"
#include "../memory/memoryAccessController.h"
#include "../memory/globalDMAClient.h"
#include "../synchronization/epochBarrierClient.h"
#include "../execution/uniqueFileDescriptor.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>

using namespace SST::Mittens;

template<class F> void rejects(F&& f) {
    bool rejected=false;
    try { f(); } catch(const std::exception&) { rejected=true; }
    assert(rejected);
}

struct MemoryFixture {
    TileConfiguration config;
    PerformanceProfile profile;
    std::uint64_t now=100, next=0;
    unsigned sent=0,retries=0,contexts=0;
    bool replay=false, segmentSafe=false;
    std::optional<QemuSyncEvent> pending;
    std::vector<std::pair<std::uint64_t,bool>> completions;
    std::unique_ptr<MemoryAccessController> memory;
    ScratchpadTimingModel* shared=nullptr;
    explicit MemoryFixture(std::uint32_t latency=3, std::uint32_t segment=1,
                           std::uint32_t hitCycles=1) {
        config.scratchpadBytes=4096;
        config.instructionFetchSegmentSize=segment;
        config.instructionCacheHitCycles=hitCycles;
        ScratchpadTimingConfiguration spm;
        spm.capacityBytes=4096; spm.latencyCycles=latency;
        auto scheduler=std::make_unique<ScratchpadTimingModel>(spm);
        shared=scheduler.get(); // Same construction-time service borrow as RX/TX.
        MemoryAccessController::Host host;
        host.now=[this] { return Timing::Ticks{now}; };
        host.instructionSegmentSafe=[this] { return segmentSafe; };
        memory=std::make_unique<MemoryAccessController>(config,Timing::Clock<Timing::Cpu>(10),
            std::move(scheduler),profile,DeviceDiagnostics{},std::move(host));
    }
    CpuMemoryAction action(std::uint64_t step,bool write=false,std::uint64_t address=0x80001000) {
        pending=QemuSyncEvent{};
        pending->stopReason=MITTENS_SYNC_STOP_MEMORY_ACCESS;
        pending->memoryAddress=address; pending->memorySize=4;
        pending->memoryFlags=write ? MITTENS_SYNC_MEMORY_FLAG_WRITE : 0;
        return {address,0x100,0x200,4,pending->memoryFlags,{}, {0},step};
    }
};

void instructionSegments() {
    for (unsigned hitCycles : {1, 3}) {
        MemoryFixture f(3,16,hitCycles);
        f.segmentSafe=true;
        std::uint64_t step=0;
        auto fetch=[&](std::uint64_t offset,unsigned proposed) {
            CpuInstructionAction action{MITTENS_SCRATCHPAD_BASE+offset,4,false,{0},++step,proposed};
            const auto result=f.memory->executeInstruction(action);
            assert(!result.complete && result.delay);
            f.now+=10*result.delay->value;
            assert(f.memory->executeInstruction(action).complete);
            return result;
        };
        assert(fetch(0,16).instructionCount==1); // Cold miss never groups.
        const auto before=f.memory->instructionStatistics();
        const auto stalls=f.memory->instructionStallCycles();
        auto group=fetch(0,16);
        assert(group.instructionCount==16 && group.delay->value==16*hitCycles);
        assert(f.memory->instructionStatistics().hits==before.hits+16);
        assert(f.memory->instructionStallCycles()==stalls+16*(hitCycles-1));
        assert(fetch(60,16).instructionCount==1); // Stop BEFORE uncached line.
        assert(f.memory->instructionStatistics().misses==1);
        f.segmentSafe=false;
        assert(fetch(0,16).instructionCount==1); // An active writer vetoes grouping.
        CpuInstructionAction invalid{MITTENS_SCRATCHPAD_BASE,4,false,{0},++step,17};
        rejects([&] { f.memory->executeInstruction(invalid); });
        invalid.count=0;
        rejects([&] { f.memory->executeInstruction(invalid); });
        invalid.count=2; invalid.bytes=2;
        rejects([&] { f.memory->executeInstruction(invalid); });
    }
}

void memoryRequests() {
    MemoryFixture f;
    auto oldRAM=f.action(1);
    rejects([&] { f.memory->executeCpuMemory(oldRAM); });
    auto invalid=f.action(2,false,MITTENS_SCRATCHPAD_BASE+4096);
    invalid.flags=MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD;
    rejects([&] { f.memory->executeCpuMemory(invalid); });
    assert(f.memory->pendingCount()==0);
}

void scratchpadDeadlineAndSharing() {
    MemoryFixture f(100);
    auto access=f.action(11,false,MITTENS_SCRATCHPAD_BASE);
    access.flags=MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD;
    auto first=f.memory->executeCpuMemory(access);
    assert(!first.complete && first.delay && first.delay->value==100);
    const auto deadline=f.now+10*first.delay->value;
    access.cursor=*first.cursor;
    f.now=deadline-1;
    auto early=f.memory->executeCpuMemory(access);
    assert(!early.complete && early.delay->value==1);
    assert(f.memory->scratchpadStatistics().cpuRequests==1);
    auto stale=access; ++stale.step;
    rejects([&] { f.memory->executeCpuMemory(stale); });
    assert(f.memory->scratchpadStatistics().cpuRequests==1);
    f.now=deadline;
    assert(f.memory->executeCpuMemory(access).complete);
    // Independent clients contend against the SAME resource; no clone/getter.
    f.shared->scheduleDMA(100,0,32,false,ScratchpadDMAClient::NetworkTransmit);
    f.shared->scheduleDMA(100,0,32,true,ScratchpadDMAClient::NetworkReceive);
    f.memory->reserveDMA(100,0,32,true);
    MittensSyncMemoryAccess run{};
    run.address=MITTENS_SCRATCHPAD_BASE; run.size=4; run.repeat_count=4;
    f.memory->reserveCPU(run,100);
    const auto stats=f.memory->scratchpadStatistics();
    assert(stats.dmaTransfers==3 && stats.dmaBytes==96 && stats.cpuRequests==5);
}

void globalDMA() {
    TileConfiguration config;
    config.tileId=3; config.globalRAMBytes=4096; config.scratchpadBytes=4096;
    std::uint64_t now=100;
    unsigned wakes=0,reservations=0;
    std::optional<QemuSyncEvent> pending=QemuSyncEvent{};
    pending->stopReason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT;
    std::vector<GlobalDMAMessage> sent;
    GlobalDMAClient::Host host;
    host.now=[&] { return Timing::Ticks{now}; };
    host.scratchpadAvailable=[] { return true; };
    host.reserveDMA=[&](std::uint64_t cursor,std::uint64_t,std::uint64_t,bool) {
        ++reservations; return ScratchpadSchedule{cursor,cursor+10,10,0,0};
    };
    host.send=[&](GlobalDMAMessage message) { sent.push_back(message); };
    host.pending=[&] { return pending; }; host.wake=[&] { ++wakes; };
    GlobalDMAClient dma(config,Timing::Clock<Timing::Cpu>(7),{},host);
    CpuGlobalDMAAction submit{MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT,0,0,1,32,0,99,0,0,8,{}, {4}};
    assert(dma.executeCpuGlobalDMA(submit).complete);
    auto wait=submit; wait.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT;
    assert(!dma.executeCpuGlobalDMA(wait).complete);
    auto waitAny=submit; waitAny.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_ANY;
    auto unready=dma.executeCpuGlobalDMA(waitAny);
    assert(!unready.complete && !unready.delay && unready.dmaCompletionStatus==0);
    auto ack=sent.front(); ack.markCompletion();
    auto wrong=GlobalDMAMessage(3,99,1,9,0,0,32,GlobalDMADirection::GlobalRAMToScratchpad,0,true);
    rejects([&] { dma.onCompletion(wrong); });
    dma.onCompletion(ack);
    assert(wakes==1 && dma.statistics().completed==1);
    rejects([&] { dma.onCompletion(ack); });
    auto delayed=dma.executeCpuGlobalDMA(wait);
    assert(!delayed.complete && delayed.delay->value==10);
    now=169;
    assert(!dma.executeCpuGlobalDMA(wait).complete && dma.statistics().pending==1);
    auto waitingForSPM=dma.executeCpuGlobalDMA(waitAny);
    assert(!waitingForSPM.complete && waitingForSPM.delay->value==1 &&
           waitingForSPM.dmaCompletionStatus==0);
    now=170;
    auto anyReady=dma.executeCpuGlobalDMA(waitAny);
    assert(anyReady.complete && anyReady.dmaCompletionStatus==1 && dma.statistics().pending==1);
    auto query=submit; query.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_QUERY;
    auto queried=dma.executeCpuGlobalDMA(query);
    assert(queried.complete && queried.dmaCompletionStatus==1 && !queried.delay);
    assert(dma.executeCpuGlobalDMA(query).dmaCompletionStatus==1);
    assert(dma.statistics().pending==1);
    query.execution=100;
    assert(dma.executeCpuGlobalDMA(query).dmaCompletionStatus==2);
    query.execution=99;
    now=169;
    assert(dma.executeCpuGlobalDMA(query).dmaCompletionStatus==0);
    assert(dma.statistics().pending==1);
    now=170;
    auto done=dma.executeCpuGlobalDMA(wait);
    assert(done.complete && done.cursor->value==14 && dma.drained());
    rejects([&] { dma.executeCpuGlobalDMA(wait); });
    rejects([&] { dma.onCompletion(ack); });

    // Queries never retire; ack cannot retire pending work, and identities
    // remain distinct even when two executions use the same token number.
    now=180;
    auto probe=submit; probe.execution=101;
    dma.executeCpuGlobalDMA(probe);
    auto poll=probe; poll.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_QUERY;
    auto retire=probe; retire.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_ACK;
    assert(dma.executeCpuGlobalDMA(poll).dmaCompletionStatus==0);
    assert(dma.executeCpuGlobalDMA(retire).dmaCompletionStatus==0);
    assert(dma.statistics().pending==1);
    auto response=sent.back(); response.markCompletion(); dma.onCompletion(response);
    now=250;
    assert(dma.executeCpuGlobalDMA(poll).dmaCompletionStatus==1);
    assert(dma.executeCpuGlobalDMA(retire).dmaCompletionStatus==1);
    assert(dma.drained());
    assert(dma.executeCpuGlobalDMA(poll).dmaCompletionStatus==2);
    assert(dma.executeCpuGlobalDMA(retire).dmaCompletionStatus==2);
    sent.pop_back(); // Keep the following batch oracle's original indices.

    // A batch retires only after every controller ack AND the latest local deadline.
    now=200; submit.token=10; submit.iteration=20;
    dma.executeCpuGlobalDMA(submit);
    now=210; submit.token=11; submit.iteration=21;
    dma.executeCpuGlobalDMA(submit);
    CpuGlobalDMAAction batch=submit;
    batch.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH; batch.token=10;
    for(unsigned i=0;i<2;++i) {
        MittensSyncGlobalDMASubmit record{};
        record.execution_id=99; record.token_id=10+i; record.logical_iteration=20+i;
        batch.waits.push_back(record);
    }
    auto ack1=sent[1]; ack1.markCompletion(); dma.onCompletion(ack1);
    assert(!dma.executeCpuGlobalDMA(batch).complete);
    auto ack2=sent[2]; ack2.markCompletion(); dma.onCompletion(ack2);
    auto batchDelay=dma.executeCpuGlobalDMA(batch);
    assert(batchDelay.delay->value==10);
    now=279; assert(!dma.executeCpuGlobalDMA(batch).complete);
    now=280; assert(dma.executeCpuGlobalDMA(batch).complete && dma.drained());
    assert(dma.statistics().submitted==4 && dma.statistics().completed==4);
    // Exact teardown is protocol work, excluded from physical DMA counters/SPM.
    submit.token=12; submit.bytes=0; submit.iteration=UINT64_MAX; submit.direction=1;
    submit.requestFlags=GlobalDMAExactReadiness|GlobalDMAExactExecutionTeardown;
    dma.executeCpuGlobalDMA(submit);
    auto teardown=sent.back(); teardown.markCompletion(); dma.onCompletion(teardown);
    wait=submit; wait.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT;
    assert(dma.executeCpuGlobalDMA(wait).complete && dma.drained());
    assert(reservations==4 && dma.statistics().submitted==4 && dma.statistics().completed==4);
}

void barriers() {
    TileConfiguration config;
    config.tileId=2;
    config.epochBarrierEpochs=2;
    config.epochBarrierDrainAnalog=true;
    bool drained=true;
    bool analogDrained=true;
    unsigned epochArrivals=0,epochWakes=0,waits=0;
    std::optional<QemuSyncEvent> pending=QemuSyncEvent{};
    EpochBarrierClient::Host host;
    host.running=[] { return true; }; host.globalDMADrained=[&] { return drained; };
    host.analogDrained=[&] { return analogDrained; };
    host.pending=[&] { return pending; }; host.completeWait=[&] { ++waits; };
    host.sendEpoch=[&](std::uint32_t,EpochBarrierContribution) { ++epochArrivals; };
    host.wakeEpoch=[&] { ++epochWakes; };
    EpochBarrierClient client(config,{},host);
    CpuBarrierAction epoch{0,MITTENS_SYNC_EPOCH_WORK_COMPLETE,MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION};
    pending->stopReason=MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE; pending->epochId=0;
    drained=false;
    rejects([&] { client.executeCpuBarrier(epoch); });
    assert(epochArrivals==0);
    drained=true;
    analogDrained=false;
    assert(!client.executeCpuBarrier(epoch).complete && epochArrivals==0);
    client.onAnalogProgress(); assert(epochWakes==0);
    analogDrained=true;
    client.onAnalogProgress(); client.onAnalogProgress(); assert(epochWakes==1);
    assert(!client.executeCpuBarrier(epoch).complete && epochArrivals==1);
    assert(!client.executeCpuBarrier(epoch).complete && epochArrivals==1);
    rejects([&] { client.onEpochRelease(2,1,EpochBarrierMessage::Release,EpochBarrierContribution::None); });
    assert(!client.onEpochRelease(2,0,EpochBarrierMessage::Release,EpochBarrierContribution::None));
    rejects([&] { client.onEpochRelease(2,0,EpochBarrierMessage::Release,EpochBarrierContribution::None); });
    assert(epochWakes==2 && client.executeCpuBarrier(epoch).complete);
    epoch.epoch=1; pending->epochId=1;
    assert(!client.executeCpuBarrier(epoch).complete);
    assert(client.onEpochRelease(2,1,EpochBarrierMessage::PrefixStop,EpochBarrierContribution::None));
    assert(waits==1 && client.snapshot().expectedEpochBarrier_==2 && client.snapshot().epochBarrierReleases_==2);
    client.validateExit();
}

struct GuestAnalogMapping {
    MittensAnalogBridgeHeader* data;
    std::size_t size;
    explicit GuestAnalogMapping(int fd):size(mittens_analog_bridge_size(1,2,2)) {
        auto* address=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        assert(address!=MAP_FAILED); data=static_cast<MittensAnalogBridgeHeader*>(address);
    }
    ~GuestAnalogMapping() { munmap(data,size); }
    void publish(std::uint32_t sequence) {
        auto* slot=mittens_analog_slot(data,0,sequence);
        slot->sequence=sequence;
        slot->command={MITTENS_ANALOG_OPERATION_LOAD_VECTOR,0,0,0};
        slot->input_word_count=2;
        auto* words=mittens_analog_slot_words(slot); words[0]=1; words[1]=2;
        mittens_analog_store_release(&slot->state,MITTENS_ANALOG_SLOT_SUBMITTED);
        mittens_analog_store_release(&mittens_analog_channel(data,0)->write_index,sequence+1);
    }
};

void analogOwnership() {
    TileConfiguration config; config.analogArrayCount=1; config.analogArrayRows=2;
    config.analogArrayColumns=2; config.analogBackend="timing"; config.qemuLocalLookahead=true;
    PerformanceProfile profile;
    std::uint64_t now=0,step=5;
    unsigned completed=0;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> wakes;
    std::optional<QemuSyncEvent> pending=QemuSyncEvent{};
    pending->stopReason=MITTENS_SYNC_STOP_ANALOG_SUBMIT;
    pending->flags=MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH;
    AnalogController::Host host;
    host.now=[&] { return Timing::Ticks{now}; }; host.active=[] { return true; };
    host.pending=[&] { return pending; }; host.pendingStepId=[&] { return step; };
    host.hasAnalogReplay=[] { return false; };
    host.completeDevice=[&](std::uint64_t id) { assert(id==step); ++completed; pending.reset(); };
    host.scheduleWake=[&](Timing::Cycles<Timing::Analog> delay,std::uint64_t generation) { wakes.emplace_back(now+delay.value*7,generation); };
    AnalogController analog(config,Timing::Clock<Timing::Analog>(7),profile,{},host);
    analog.setup();
    GuestAnalogMapping guest(analog.fileDescriptor());
    guest.publish(0);
    assert(analog.startDeferredAnalogLoadLookahead(*pending));
    assert(analog.hasDeferred() && analog.deferredMatches(*pending));
    assert(analog.statistics().submitted==0);
    guest.publish(1); // Lookahead publication must not be consumed early.
    analog.serviceAnalogBridge();
    assert(analog.statistics().submitted==0 && analog.submitted(0,1));
    CpuAnalogAction submit{MITTENS_SYNC_STOP_ANALOG_SUBMIT,0,MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH,0};
    assert(analog.executeCpuAnalog(submit).complete);
    assert(analog.statistics().submitted==1 && analog.submitted(0,1));
    pending->stopReason=MITTENS_SYNC_STOP_ANALOG_WAIT;
    pending->flags=MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION;
    assert(!wakes.empty());
    const auto initial=wakes.front();
    analog.onWake(initial.second+100); // stale generation
    analog.onWake(initial.second); // correct generation, wrong tick
    assert(completed==0);
    for(std::size_t i=0; i<wakes.size() && completed==0; ++i) {
        assert(i<20); now=wakes[i].first; analog.onWake(wakes[i].second);
    }
    assert(completed==1 && analog.statistics().completed==1 && analog.submitted(0,1));
    analog.onWake(initial.second);
    assert(completed==1);
    auto stats=analog.statistics(); stats.inputWords=999;
    assert(stats.inputWords==999 && analog.statistics().inputWords==2);
    analog.close(); analog.close();
}

void descriptorLifetime() {
    int fd=-1;
    struct Failure {
        UniqueFileDescriptor descriptor;
        explicit Failure(int& observed):descriptor(::open("/dev/null",O_RDONLY)) {
            observed=descriptor.get(); assert(observed>=0);
            throw std::runtime_error("injected constructor failure");
        }
    };
    rejects([&] { Failure failure(fd); });
    errno=0; assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
    {
        UniqueFileDescriptor first(::open("/dev/null",O_RDONLY)); fd=first.get();
        UniqueFileDescriptor second(std::move(first));
        assert(first.get()==-1 && second.get()==fd);
        second.reset(fd); assert(fcntl(fd,F_GETFD)!=-1);
        second.reset(); second.reset();
    }
    errno=0; assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
}

int main() {
    memoryRequests(); scratchpadDeadlineAndSharing(); instructionSegments(); globalDMA(); barriers();
    analogOwnership(); descriptorLifetime();
    std::cout<<"device owners: memory/groups/SPM/deadlines/globalDMA/barriers/analog/RAII PASS\n";
}
