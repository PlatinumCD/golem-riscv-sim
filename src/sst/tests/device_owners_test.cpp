#include "../analog/analogController.h"
#include "../memory/memoryAccessController.h"
#include "../memory/globalDMAClient.h"
#include "../synchronization/initializationBarrierClient.h"
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
    struct Request final : MemoryAccessController::PreparedRequest {
        std::uint64_t number;
        unsigned& sent;
        Request(std::uint64_t n,unsigned& s):number(n),sent(s) {}
        std::uint64_t id() const noexcept override { return number; }
        void send() override { ++sent; }
    };
    TileConfiguration config;
    PerformanceProfile profile;
    std::uint64_t now=100, next=0;
    unsigned sent=0,retries=0,contexts=0;
    bool replay=false;
    std::optional<QemuSyncEvent> pending;
    std::vector<std::pair<std::uint64_t,bool>> completions;
    std::unique_ptr<MemoryAccessController> memory;
    ScratchpadTimingModel* shared=nullptr;
    explicit MemoryFixture(std::uint32_t latency=3) {
        config.memoryStoreBufferEntries=2;
        config.memoryTileStride=0;
        config.scratchpadBytes=4096;
        config.memoryCacheLineSize=64;
        ScratchpadTimingConfiguration spm;
        spm.capacityBytes=4096; spm.latencyCycles=latency;
        auto scheduler=std::make_unique<ScratchpadTimingModel>(spm);
        shared=scheduler.get(); // Same construction-time service borrow as RX/TX.
        MemoryAccessController::Host host;
        host.now=[this] { return Timing::Ticks{now}; };
        host.context=[this] { ++contexts; return MemoryAccessController::Context{7,91,"task"}; };
        host.prepare=[this](std::uint64_t,std::uint32_t,bool) { return std::make_unique<Request>(++next,sent); };
        host.pending=[this] { return pending; };
        host.hasMemoryReplay=[this] { return replay; };
        host.completeMemory=[this](std::uint64_t step,bool group) { completions.emplace_back(step,group); };
        host.retry=[this] { ++retries; return false; };
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

void memoryRequests() {
    MemoryFixture f;
    auto first=f.action(1,true);
    auto result=f.memory->executeCpuMemory(first);
    assert(result.complete && result.reportBlockedAfterCompletion && f.sent==1);
    auto second=f.action(2,true,0x80002000);
    assert(!f.memory->executeCpuMemory(second).complete && f.sent==2);
    f.memory->onResponse(1); // Frees full buffer; retires the CURRENT blocking store.
    assert((f.completions==std::vector<std::pair<std::uint64_t,bool>>{{2,false}}));
    f.pending->stopReason=MITTENS_SYNC_STOP_MEMORY_FENCE;
    f.memory->onResponse(2); // Released store's later ack only retries, never retires.
    assert(f.completions.size()==1 && f.retries==1 && f.memory->storesDrained());
    rejects([&] { f.memory->onResponse(2); });
    assert(f.memory->statistics().memoryResponses_==2 && f.memory->pendingCount()==0);
    assert(f.contexts==2);
    auto snapshot=f.memory->statistics(); snapshot.memoryRequests_=100;
    assert(snapshot.memoryRequests_==100 && f.memory->statistics().memoryRequests_==2);

    auto store=f.action(3,true);
    assert(f.memory->executeCpuMemory(store).complete);
    auto load=f.action(4);
    assert(!f.memory->executeCpuMemory(load).complete && f.sent==3); // RAW hazard.
    f.memory->onResponse(3);
    assert(f.retries==2);
    assert(!f.memory->executeCpuMemory(load).complete && f.sent==4);
    f.memory->onResponse(4);
    assert(f.completions.back()==std::make_pair(std::uint64_t(4),false));

    f.replay=true;
    auto group=f.action(5);
    MittensSyncMemoryAccess a{};
    a.address=0x80003000; a.size=4; a.repeat_count=1; a.program_counter=0x123;
    a.return_address=0x456;
    auto b=a; b.address+=64;
    group.group={a,b};
    assert(!f.memory->executeCpuMemory(group).complete && f.sent==6);
    f.memory->onResponse(6);
    assert(f.completions.size()==2 && f.memory->pendingCount()==1);
    assert(!f.memory->executeCpuMemory(group).complete && f.sent==6);
    f.memory->onResponse(5);
    assert(f.completions.back()==std::make_pair(std::uint64_t(5),true));
    rejects([&] { f.memory->onResponse(5); });
    auto bad=f.action(6); bad.size=0;
    rejects([&] { f.memory->executeCpuMemory(bad); });
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
    now=170;
    auto done=dma.executeCpuGlobalDMA(wait);
    assert(done.complete && done.cursor->value==14 && dma.drained());
    rejects([&] { dma.executeCpuGlobalDMA(wait); });
    rejects([&] { dma.onCompletion(ack); });

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
    assert(dma.statistics().submitted==3 && dma.statistics().completed==3);
    // Exact teardown is protocol work, excluded from physical DMA counters/SPM.
    submit.token=12; submit.bytes=0; submit.iteration=UINT64_MAX; submit.direction=1;
    submit.requestFlags=GlobalDMAExactReadiness|GlobalDMAExactExecutionTeardown;
    dma.executeCpuGlobalDMA(submit);
    auto teardown=sent.back(); teardown.markCompletion(); dma.onCompletion(teardown);
    wait=submit; wait.reason=MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT;
    assert(dma.executeCpuGlobalDMA(wait).complete && dma.drained());
    assert(reservations==3 && dma.statistics().submitted==3 && dma.statistics().completed==3);
}

void barriers() {
    TileConfiguration config;
    config.tileId=2; config.memoryInitializationBatching=true;
    config.memoryInitializationBytesPerCycle=16; config.memoryInitializationLatencyCycles=3;
    config.memoryInitializationBarrierTiles=4; config.epochBarrierEpochs=2;
    bool drained=true;
    unsigned initArrivals=0,epochArrivals=0,initWakes=0,epochWakes=0,waits=0;
    std::optional<QemuSyncEvent> pending=QemuSyncEvent{};
    pending->stopReason=MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE;
    InitializationBarrierClient::Host host;
    host.running=[] { return true; }; host.globalDMADrained=[&] { return drained; };
    host.pending=[&] { return pending; }; host.completeWait=[&] { ++waits; };
    host.sendInitialization=[&] { ++initArrivals; }; host.sendEpoch=[&](std::uint32_t,EpochBarrierContribution) { ++epochArrivals; };
    host.wakeInitialization=[&] { ++initWakes; }; host.wakeEpoch=[&] { ++epochWakes; };
    InitializationBarrierClient client(config,{},host);
    client.start();
    rejects([&] { client.onInitializationRelease(2,MemoryInitializationBarrierMessage::Release); });
    CpuInitializationAction init{4,17,16,0,0};
    auto delay=client.executeCpuInitialization(init);
    assert(delay.delay->value==6 && client.snapshot().memoryInitializationHandshakes_==1);
    assert(!client.executeCpuInitialization(init).complete && initArrivals==1);
    assert(!client.executeCpuInitialization(init).complete && initArrivals==1);
    rejects([&] { client.onInitializationRelease(1,MemoryInitializationBarrierMessage::Release); });
    client.onInitializationRelease(2,MemoryInitializationBarrierMessage::Release);
    rejects([&] { client.onInitializationRelease(2,MemoryInitializationBarrierMessage::Release); });
    assert(initWakes==1 && client.executeCpuInitialization(init).complete);
    assert(!client.snapshot().memoryInitializationPhase_);

    CpuBarrierAction epoch{0,MITTENS_SYNC_EPOCH_WORK_COMPLETE,MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION};
    pending->stopReason=MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE; pending->epochId=0;
    drained=false;
    rejects([&] { client.executeCpuBarrier(epoch); });
    assert(epochArrivals==0);
    drained=true;
    assert(!client.executeCpuBarrier(epoch).complete && epochArrivals==1);
    assert(!client.executeCpuBarrier(epoch).complete && epochArrivals==1);
    rejects([&] { client.onEpochRelease(2,1,EpochBarrierMessage::Release,EpochBarrierContribution::None); });
    assert(!client.onEpochRelease(2,0,EpochBarrierMessage::Release,EpochBarrierContribution::None));
    rejects([&] { client.onEpochRelease(2,0,EpochBarrierMessage::Release,EpochBarrierContribution::None); });
    assert(epochWakes==1 && client.executeCpuBarrier(epoch).complete);
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
    memoryRequests(); scratchpadDeadlineAndSharing(); globalDMA(); barriers();
    analogOwnership(); descriptorLifetime();
    std::cout<<"device owners: memory/groups/SPM/deadlines/globalDMA/barriers/analog/RAII PASS\n";
}
