#include "../execution/cpuExecutionController.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <thread>
using namespace SST::Mittens;
struct Fixture {
    TileConfiguration config;
    std::unique_ptr<CpuExecutionController> cpu;
    std::deque<QemuSyncEvent> captures;
    std::deque<std::uint64_t> wakes;
    std::deque<std::uint64_t> wakeGenerations;
    std::vector<std::uint32_t> actions;
    std::uint64_t now=0, epoch=0, resumes=0;
    std::uint64_t cpuTicks=1, memoryDelay=0;
    bool running=true, initializing=false, block=false;
    std::atomic<bool> workerEntered{false}, workerExited{false};
    bool waitWorker=false;
    bool buffered=false, invalidResult=false, reenter=false;
    unsigned watchdogCalls=0, initialValidations=0;
    ~Fixture() { if(cpu) cpu->stop(); }
    Fixture() {
        config.cpuIssueWidth=4; config.syncInstructionQuantum=100;
        config.memoryInitializationInstructionQuantum=100;
        config.memoryInitializationBarrierTiles=1; config.qemuReadySetWorkers=2;
        config.scratchpadAccessBatching=true; config.scratchpadBytes=4096;
        config.globalDMASubmitBatching=true; config.globalDMAMacroExecution=true;
        config.analogCommandBatching=true;
    }
    QemuSyncEvent take() {
        assert(!captures.empty()); auto event=captures.front(); captures.pop_front(); return event;
    }
    void create() {
        CpuExecutionController::Transport t;
        t.grant=[this](std::uint64_t) { return ++epoch; };
        t.resume=[this](const QemuSyncEvent&) { ++resumes; };
        t.poll=[this]() -> std::optional<QemuSyncEvent> { return take(); };
        t.captureHost=[this](const std::atomic<bool>& active) {
            workerEntered=true;
            if(waitWorker) {
                while(active.load()) std::this_thread::yield();
                workerExited=true; throw std::runtime_error("cancelled fake capture");
            }
            workerExited=true; return take();
        };
        CpuExecutionController::Host h;
        h.running=[this]{return running;}; h.initializing=[this]{return initializing;};
        h.observeExit=[] {return false;}; h.watchdogReported=[] {return false;};
        h.now=[this]{return Timing::Ticks{now};}; h.watchdog=[this]{++watchdogCalls;}; h.terminateAll=[]{};
        h.scheduleCaptureDispatch=[]{}; h.serviceBridge=[]{};
        h.schedule=[this](auto cycles,auto,auto generation){wakes.push_back(cycles.value);wakeGenerations.push_back(generation);}; h.scheduleWatchdog=[](auto,auto){};
        h.progress=[](bool){}; h.recordWait=[](auto,auto,const char*,auto){};
        h.log=[](int,const std::string&){};
        h.scratchpadAvailable=[] {return true;}; h.storesDrained=[] {return true;};
        h.receiveReady=[] {return false;}; h.transmitReady=[](bool){return false;};
        h.scratchpad=[](const auto&,std::uint64_t cursor) {ScratchpadSchedule s{}; s.completionCycle=cursor+2; s.serviceCycles=2; return s;};
        h.analogArrayCount=[] {return 2U;}; h.analogSubmitted=[](auto,auto){return true;};
        h.prepareDeferredAnalog=[](const auto&){return false;}; h.deferredAnalogMatches=[](const auto&){return false;};
        h.validateInitialDevice=[this](const auto&){++initialValidations;};
        h.memory=[this](const auto& a){
            actions.push_back(MITTENS_SYNC_STOP_MEMORY_ACCESS); assert(a.group.size()<=2);
            if(memoryDelay) {const auto delay=memoryDelay; memoryDelay=0;
                return CpuDeviceResult{false,Timing::Cycles<Timing::Cpu>{delay},{}};}
            if(reenter) cpu->completeMemory(cpu->pendingStepId());
            return CpuDeviceResult{!block,invalidResult?std::optional<Timing::Cycles<Timing::Cpu>>{{1}}:std::nullopt,{},buffered};
        };
        h.analog=[this](const auto& a){actions.push_back(a.reason); return CpuDeviceResult{!block,{},{}};};
        h.globalDMA=[this](const auto& a){actions.push_back(a.reason); return CpuDeviceResult{!block,{},{}};};
        h.network=[](const auto&){return CpuDeviceResult{true,{},{}};};
        h.barrier=[](const auto&){return CpuDeviceResult{true,{},{}};};
        h.initialization=[](const auto&){return CpuDeviceResult{true,{},{}};};
        h.task=[](const auto&){return CpuDeviceResult{true,{},{}};};
        h.guestExit=[this]{running=false; cpu->stop();};
        cpu=std::make_unique<CpuExecutionController>(config,Timing::Clock<Timing::Cpu>(cpuTicks),std::move(t),std::move(h),0,config.qemuReadySetWorkers);
    }
    void wake() {assert(!wakes.empty());now+=wakes.front()*cpuTicks;wakes.pop_front();auto generation=wakeGenerations.front();wakeGenerations.pop_front();cpu->onWake({},generation);}
};
static QemuSyncEvent event(std::uint64_t i,std::uint64_t v,std::uint32_t reason,std::uint64_t epoch=1) {
    QemuSyncEvent e{}; e.instructionsExecuted=i; e.vectorInstructionsExecuted=v;
    e.stopReason=reason;e.grantEpoch=epoch;e.eventSequence=i+1;return e;
}
static MittensSyncMemoryAccess access(std::uint64_t i,std::uint64_t v,bool scratchpad=true) {
    MittensSyncMemoryAccess a{};a.instructions_executed=i;a.vector_instructions_executed=v;
    a.address=scratchpad ? MITTENS_SCRATCHPAD_BASE : 0x80000000;a.size=4;a.repeat_count=1;
    a.flags=scratchpad ? MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD : 0;return a;
}
static void ordinary() {
    Fixture f; f.config.qemuReadySetWorkers=1; f.create();
    f.captures={event(3,1,MITTENS_SYNC_STOP_QUANTUM_END),event(2,0,MITTENS_SYNC_STOP_TASK_FINISH,2),event(3,0,MITTENS_SYNC_STOP_GUEST_EXIT,2)};
    f.cpu->onWake(); f.wake(); f.wake(); f.wake();
    assert(!f.running && f.cpu->accounting().total.instructions==6 && f.cpu->accounting().totalCycles==3);
}
static void memoryTail(bool fused,bool quantum) {
    Fixture f; f.config.qemuReadySetWorkers=1;f.create();
    auto e=event(9,1,fused?MITTENS_SYNC_STOP_MEMORY_FENCE:MITTENS_SYNC_STOP_MEMORY_BATCH);
    e.flags=fused?MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH:quantum?MITTENS_SYNC_EVENT_FLAG_QUANTUM_END:0;
    e.memoryBatch={access(2,0),access(4,1)};
    f.captures.push_back(e);f.captures.push_back(event(quantum?1:10,quantum?0:1,MITTENS_SYNC_STOP_GUEST_EXIT,quantum?2:1));
    f.cpu->onWake(); assert(f.cpu->accounting().totalCycles==4); assert(f.wakes.front()==8);
    f.wake();f.wake(); assert(!f.running);
    assert(f.cpu->accounting().totalCycles==(fused?5:4));
    assert(f.cpu->statistics().memoryBatchLogicalAccesses_==2);
}
static void analogTail(bool fused) {
    Fixture f;f.config.qemuReadySetWorkers=1;f.create();
    auto e=event(7,0,fused?MITTENS_SYNC_STOP_MEMORY_FENCE:MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH);
    e.flags=fused?MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH:0;
    MittensSyncAnalogSubmit a{};a.instructions_executed=2;a.array_id=0;a.sequence=1;e.analogSubmitBatch.push_back(a);
    f.captures={e,event(8,0,MITTENS_SYNC_STOP_GUEST_EXIT)};
    f.cpu->onWake();f.wake(); assert(f.cpu->accounting().totalCycles==3);
    f.wake();f.wake();assert(f.cpu->accounting().totalCycles==(fused?4:3));
    assert(f.actions.size()==1);
}
static void dma(bool macro,bool fusedWait) {
    Fixture f;f.config.qemuReadySetWorkers=1;f.create();
    auto e=event(11,0,macro?MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO:fusedWait?MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH:MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH);
    e.flags=fusedWait?MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS:0;e.executionId=4;
    for(unsigned j=0;j<2;++j) {MittensSyncGlobalDMASubmit a{};a.instructions_executed=2+j*4;a.wait_instructions_executed=4+j*4;
        a.execution_id=4;a.token_id=j;a.submit_event_ordinal=j*2;a.wait_event_ordinal=j*2+1;e.globalDMASubmitBatch.push_back(a);}
    f.captures={e,event(12,0,MITTENS_SYNC_STOP_GUEST_EXIT)};
    f.cpu->onWake();
    if(macro) {for(int n=0;n<4;++n)f.wake(); assert(f.cpu->accounting().baseline.instructions==8);assert(f.wakes.front()==0);}
    else {f.wake();f.wake();assert(f.cpu->accounting().baseline.instructions==11);}
    f.wake();f.wake();assert(!f.running && f.cpu->accounting().totalCycles==(macro?5:4));
    assert(f.actions.size()==(macro?4:fusedWait?3:2));
}
static void grouped(bool scalar) {
    Fixture f;f.config.qemuReadySetWorkers=1;f.block=true;f.create();
    auto e=event(7,scalar?0:1,MITTENS_SYNC_STOP_MEMORY_BATCH);auto a=access(2,scalar?0:1,false),b=a;b.address+=64;
    if(scalar){a.flags=b.flags=MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS;a.program_counter=100;b.program_counter=104;a.instruction_length=b.instruction_length=4;b.instructions_executed=3;}
    e.memoryBatch={a,b};
    f.captures={e,event(8,scalar?0:1,MITTENS_SYNC_STOP_GUEST_EXIT)};f.cpu->onWake();f.wake();
    const auto charged=f.cpu->accounting().totalCycles;f.cpu->processPendingSyncEvent();
    assert(f.cpu->accounting().totalCycles==charged);f.cpu->completeMemory(f.cpu->pendingStepId(),true);f.wake();f.wake();
    assert(!f.running && f.cpu->accounting().totalCycles==3);
}
static void runtimeCancel(bool collected) {
    Fixture f; f.config.qemuRuntimeReadySet=true;
    QemuCaptureCoordinator::registerRuntimeQemuReadySetTile(0,2,1,"test"); f.create();
    f.captures={event(2,0,MITTENS_SYNC_STOP_MEMORY_FENCE),event(3,0,MITTENS_SYNC_STOP_GUEST_EXIT)};
    f.cpu->onWake();f.wake();assert(f.cpu->hasPendingWork() && !f.cpu->pending());
    if(collected) {
        auto completions=QemuCaptureCoordinator::dispatchRuntimeQemuReadySet(f.now);
        assert(completions.size()==1); f.cpu.reset();
        completions.front().commit(completions.front().event); // revoked: no dereference of destroyed owner
    } else {
        f.cpu->stop();assert(QemuCaptureCoordinator::dispatchRuntimeQemuReadySet(f.now).empty());
        assert(!f.workerEntered && !f.cpu->hasPendingWork());
    }
}
static void localCancel() {
    Fixture f;f.config.qemuLocalLookahead=true;f.waitWorker=true;
    QemuCaptureCoordinator::registerLocalQemuLookaheadTile(0,2,1,"test");f.create();
    auto e=event(4,0,MITTENS_SYNC_STOP_MEMORY_BATCH);e.memoryBatch={access(2,0)};f.captures={e};
    f.cpu->onWake();while(!f.workerEntered.load())std::this_thread::yield();
    f.cpu->stop();assert(f.workerExited && !f.cpu->hasPendingWork());
}
static void runtimeSuccess() {
    Fixture f;f.config.qemuRuntimeReadySet=true;
    QemuCaptureCoordinator::registerRuntimeQemuReadySetTile(0,2,1,"test");f.create();
    f.captures={event(2,0,MITTENS_SYNC_STOP_MEMORY_FENCE),event(3,0,MITTENS_SYNC_STOP_GUEST_EXIT)};
    f.cpu->onWake();f.wake();
    auto completions=QemuCaptureCoordinator::dispatchRuntimeQemuReadySet(f.now);
    assert(completions.size()==1 && f.resumes==1 && f.initialValidations==0);
    auto& completion=completions.front();completion.commit(completion.event);
    const auto cycles=f.cpu->accounting().totalCycles;
    bool threw=false;
    try{completion.commit(completion.event);}catch(const std::logic_error&){threw=true;}
    assert(threw && f.cpu->accounting().totalCycles==cycles);
    f.wake();assert(!f.running && f.cpu->accounting().total.instructions==3);
}
static void initialCapture(const std::string& mode) {
    Fixture f;f.initializing=true;
    QemuCaptureCoordinator::registerInitialQemuReadySetTile(0,0,2,2,1,"test");f.create();
    const bool invalid=mode=="initial-invalid", mismatch=mode=="initial-epoch-mismatch";
    f.captures={event(2,0,invalid?MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:MITTENS_SYNC_STOP_MEMORY_FENCE)};
    if(mismatch)f.epoch=5;
    std::string error;
    try{f.cpu->onWake();}catch(const std::exception& e){error=e.what();}
    if(invalid || mismatch) {
        assert(error.find(invalid?"unsupported event":"did not match its reservation")!=std::string::npos);
        assert(!f.cpu->pending() && f.cpu->accounting().total.instructions==0 && f.initialValidations==0);
    } else {
        assert(error.empty() && f.cpu->pending() && f.initialValidations==1);
        assert(f.cpu->accounting().epoch==1 && f.cpu->statistics().synchronizationGrants_==1);
    }
}
static void localSuccess() {
    QemuCaptureCoordinator::registerLocalQemuLookaheadTile(0,2,1,"test");
    for(bool quantum:{false,true}) {
        Fixture f;f.config.qemuLocalLookahead=true;f.create();
        auto e=event(9,1,MITTENS_SYNC_STOP_MEMORY_BATCH);
        e.flags=quantum?MITTENS_SYNC_EVENT_FLAG_QUANTUM_END:0;e.memoryBatch={access(2,0),access(4,1)};
        f.captures={e,event(quantum?1:10,quantum?0:1,MITTENS_SYNC_STOP_GUEST_EXIT,quantum?2:1)};
        f.cpu->onWake();f.wake();f.wake();
        assert(!f.running && f.cpu->accounting().totalCycles==4 && f.cpu->accounting().total.instructions==10);
    }
}
static void initialCancel() {
    Fixture f;f.initializing=true;f.waitWorker=true;f.config.memoryInitializationBarrierTiles=2;
    QemuCaptureCoordinator::registerInitialQemuReadySetTile(0,0,2,2,2,"test");
    QemuCaptureCoordinator::registerInitialQemuReadySetTile(0,1,2,2,2,"test");f.create();
    f.cpu->onWake();while(!f.workerEntered.load())std::this_thread::yield();
    assert(f.cpu->hasPendingWork() && !f.cpu->pending());f.cpu->stop();assert(f.workerExited && !f.cpu->hasPendingWork());
}
static void completionGuards() {
    Fixture f;f.config.qemuReadySetWorkers=1;f.block=true;f.create();
    auto e=event(7,0,MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH);
    for(unsigned n=0;n<2;++n){MittensSyncAnalogSubmit a{};a.instructions_executed=2+n*2;a.sequence=1+n;e.analogSubmitBatch.push_back(a);}
    f.captures={e,event(8,0,MITTENS_SYNC_STOP_GUEST_EXIT)};f.cpu->onWake();
    const auto first=f.cpu->pendingStepId();f.wake();f.cpu->completeDevice(first);
    const auto second=f.cpu->pendingStepId();assert(first!=second);
    auto cycles=f.cpu->accounting().totalCycles;
    bool threw=false;try{f.cpu->completeDevice(first);}catch(const std::logic_error&){threw=true;}
    assert(threw && f.cpu->pendingStepId()==second && f.cpu->accounting().totalCycles==cycles);
    f.wake();f.cpu->completeDevice(second);f.wake();f.wake();
    threw=false;try{f.cpu->completeDevice(second);}catch(const std::logic_error&){threw=true;}assert(threw);
}
static void callbackContract(bool reenter) {
    Fixture f;f.config.qemuReadySetWorkers=1;f.reenter=reenter;f.invalidResult=!reenter;f.create();
    auto e=event(2,0,MITTENS_SYNC_STOP_MEMORY_ACCESS);e.memorySize=4;f.captures={e};f.cpu->onWake();
    auto step=f.cpu->pendingStepId();auto cycles=f.cpu->accounting().totalCycles;
    bool threw=false;try{f.wake();}catch(const std::logic_error&){threw=true;}
    assert(threw && step==f.cpu->pendingStepId() && cycles==f.cpu->accounting().totalCycles);
}
static void bufferedWatchdog() {
    Fixture f;f.config.qemuReadySetWorkers=1;f.buffered=true;f.create();
    auto e=event(2,0,MITTENS_SYNC_STOP_MEMORY_ACCESS);e.memoryFlags=MITTENS_SYNC_MEMORY_FLAG_WRITE;e.memorySize=4;
    f.captures={e,event(3,0,MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT)};f.cpu->onWake();auto before=f.watchdogCalls;
    f.wake();assert(f.watchdogCalls==before+2); // new capture + blocked-path observation
    auto next=f.cpu->pendingStepId();bool threw=false;
    try{f.cpu->completeMemory(next-1);}catch(const std::logic_error&){threw=true;}
    assert(threw && next==f.cpu->pendingStepId());
}
// Invoke the real controller exactly as an independent buffered-store response
// does, between capture installation and the captured instruction deadline.
static bool earlyDeliveryOracle(std::uint64_t factor=1) {
    Fixture f; f.config.qemuReadySetWorkers=1; f.config.cpuIssueWidth=1;
    f.cpuTicks=factor;
    f.buffered=true; f.create();
    auto store=event(2,0,MITTENS_SYNC_STOP_MEMORY_ACCESS);
    store.memoryFlags=MITTENS_SYNC_MEMORY_FLAG_WRITE; store.memorySize=4;
    f.captures={store,event(82,0,MITTENS_SYNC_STOP_MEMORY_FENCE),
                event(86,0,MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT)};
    f.cpu->onWake(); f.wake();
    const auto fenceStep=f.cpu->pendingStepId();
    f.now=10*factor; // buffered write response, 72 CPU cycles before fence delivery
    f.cpu->processPendingSyncEvent();
    const bool pass=f.resumes==1 && f.cpu->pendingStepId()==fenceStep;
    std::cout<<"delivery oracle: response_tick=10 deadline_tick=82 resumes="
             <<f.resumes<<" expected_resumes=1 "<<(pass?"PASS":"FAIL")<<'\n';
    return pass;
}
static void deadlineEntries(std::uint64_t factor, bool memory) {
    Fixture f; f.config.qemuReadySetWorkers=1; f.config.cpuIssueWidth=1;
    f.cpuTicks=factor; f.block=true; f.create();
    auto e=event(82,0,memory?MITTENS_SYNC_STOP_MEMORY_ACCESS:MITTENS_SYNC_STOP_ANALOG_WAIT);
    e.memorySize=4;
    f.captures={e,event(86,0,MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT)};
    f.cpu->onWake(); const auto step=f.cpu->pendingStepId();
    const auto generation=f.wakeGenerations.front();
    f.now=10*factor;
    if(memory) f.cpu->completeMemory(step); else f.cpu->completeDevice(step);
    f.cpu->processPendingSyncEvent();
    f.cpu->onWake({},generation); // even an incorrectly early timer cannot deliver
    assert(f.actions.empty() && f.resumes==0 && f.cpu->pendingStepId()==step);
    f.now=82*factor; f.cpu->onWake({},generation);
    assert(f.actions.size()==1 && f.resumes==0);
    if(memory) f.cpu->completeMemory(step); else f.cpu->completeDevice(step);
    assert(f.resumes==1 && f.cpu->pendingStepId()!=step);
    auto next=f.cpu->pendingStepId();
    f.now=83*factor; f.cpu->onWake({},generation); // superseded timer
    assert(f.resumes==1 && f.cpu->pendingStepId()==next);
    f.now=100*factor; f.cpu->onWake({},generation); // stale even after next deadline
    assert(f.cpu->pendingStepId()==next && f.resumes==1);
}
static void serviceDeadline(std::uint64_t factor) {
    Fixture f; f.config.qemuReadySetWorkers=1; f.config.cpuIssueWidth=1;
    f.cpuTicks=factor; f.memoryDelay=100; f.create();
    auto e=event(2,0,MITTENS_SYNC_STOP_MEMORY_ACCESS);e.memorySize=4;
    f.captures={e,event(3,0,MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT)};
    f.cpu->onWake();f.wake(); const auto step=f.cpu->pendingStepId();
    auto generation=f.wakeGenerations.front();
    for(auto cycle:{10U,50U,101U}) {
        f.now=cycle*factor; f.cpu->processPendingSyncEvent();
        f.cpu->completeMemory(step);
        assert(f.actions.size()==1 && f.resumes==0);
    }
    f.now=102*factor;f.cpu->onWake({},generation);
    assert(f.actions.size()==2 && f.resumes==1);
    const auto next=f.cpu->pendingStepId();
    f.cpu->onWake({},generation);
    assert(f.cpu->pendingStepId()==next && f.resumes==1);
    f.cpu->stop(); f.now=1000*factor; f.cpu->onWake({},generation);
    assert(f.resumes==1);
}
static void deadlines() {
    for(auto factor:{1U,500U,1000U,2000U}) {
        assert(earlyDeliveryOracle(factor));
        deadlineEntries(factor,false);deadlineEntries(factor,true);serviceDeadline(factor);
    }
}
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"replay";
    if(mode=="delivery-oracle")return earlyDeliveryOracle()?0:1;
    if(mode=="deadlines"){deadlines();std::cout<<"CPU deadline entries: PASS\n";return 0;}
    if(mode=="runtime-cancel")runtimeCancel(false);
    else if(mode=="runtime-success")runtimeSuccess();
    else if(mode=="late-commit")runtimeCancel(true);
    else if(mode=="local-cancel")localCancel();
    else if(mode=="local-success")localSuccess();
    else if(mode=="initial-cancel")initialCancel();
    else if(mode=="initial-success" || mode=="initial-invalid" || mode=="initial-epoch-mismatch")initialCapture(mode);
    else {ordinary();memoryTail(false,false);memoryTail(false,true);memoryTail(true,false);analogTail(false);analogTail(true);dma(false,false);dma(false,true);dma(true,false);grouped(false);grouped(true);completionGuards();callbackContract(false);callbackContract(true);bufferedWatchdog();deadlines();}
    std::cout<<"CPU controller "<<mode<<": PASS\n";
}
