#include <sst/core/sst_config.h>
#include "riscvQemu.h"
#include "../cycleProfile.h"
#include "externalCommit.h"
#include "../fixed.h"
#include "../observations.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace TileComponents {

RiscvQemu::RiscvQemu(SST::ComponentId_t id, SST::Params& p)
    : Component(id), ledger_(p.find<std::uint64_t>("issue_width", 1)),
      budget_(p.find<std::uint64_t>("instruction_budget", 256)),
      capacity_(p.find<std::uint64_t>("spm_capacity_bytes", 2097152)),
      requestBytes_(p.find<std::uint64_t>("spm_request_bytes", 32)),
      timeoutSeconds_(p.find<std::uint64_t>("host_timeout_seconds", 30)) {
    try {
        // This optional single-tile observation filter changes no execution,
        // counters, memory requests or synchronization. The requested task
        // start already drains outstanding work before traceTask is called.
        if (const char* task = std::getenv("TILE_COMPONENT_TRACE_START_TASK")) {
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtoull(task, &end, 10);
            if (!*task || *task < '0' || *task > '9' || *end || errno)
                throw std::invalid_argument("TILE_COMPONENT_TRACE_START_TASK must be an unsigned decimal task ID");
            observationStartTask_ = value;
            observationTaskPending_ = true;
            RecordComponentObservations = false;
        }
        const auto ranks = getNumRanks();
        if (ranks.rank != 1 || ranks.thread != 1)
            throw std::invalid_argument("shared-SPM QEMU requires one SST rank and one thread");
        if (!budget_ || budget_ > INT64_MAX || !timeoutSeconds_ || timeoutSeconds_ > 3600 ||
            !capacity_ || capacity_ > 32 * 1024 * 1024 || capacity_ % 4096 || requestBytes_ < 4 ||
            requestBytes_ > 2 * 1024 * 1024 || (requestBytes_ & (requestBytes_ - 1)) ||
            capacity_ % requestBytes_)
            throw std::invalid_argument("invalid CPU budget, timeout, SPM capacity or request size");
        const auto depth = p.find<std::uint64_t>("load_store_queue_depth", 1);
        if (!depth || depth > MITTENS_SYNC_LSQ_CAPACITY)
            throw std::invalid_argument("load_store_queue_depth must be 1..64");
        loadStoreDepth_ = depth;
        const auto analogDepth = p.find<std::uint64_t>("analog_command_queue_depth", 0);
        analogByteCapacity_ = p.find<std::uint64_t>("analog_command_queue_bytes", 16384);
        if (analogDepth > MITTENS_SYNC_ASQ_CAPACITY || analogByteCapacity_ < 1024 ||
            analogByteCapacity_ > 16384 || analogByteCapacity_ % 4)
            throw std::invalid_argument("invalid analog command queue depth or byte capacity");
        analogDepth_ = analogDepth;
        config_.executable = p.find<std::string>("qemu", "");
        config_.elfPath = p.find<std::string>("elf", "");
        config_.memory = "16M"; // QEMU virt infrastructure; guest ELF/data must be in SPM.
        config_.scratchpadBytes = capacity_;
        config_.riscvVectorEnabled = p.find<bool>("riscv_vector_enabled", true);
        config_.riscvVectorLengthBits = p.find<unsigned>("riscv_vector_length_bits", 256);
        config_.riscvVectorElementBits = p.find<unsigned>("riscv_vector_element_bits", 64);
        config_.serialOutputPath = p.find<std::string>("serial_output", "");
        arrayPipelineEnabled_ = p.find<bool>("array_pipeline_enabled", true);
        const auto vlen = config_.riscvVectorLengthBits;
        const auto elen = config_.riscvVectorElementBits;
        if (config_.riscvVectorEnabled &&
            (vlen < 128 || vlen > 1024 || (vlen & (vlen - 1)) || (elen != 32 && elen != 64)))
            throw std::invalid_argument("invalid RVV VLEN/ELEN");
        auto cache = std::make_unique<Riscv::InstructionCache>(config_.scratchpadBase, capacity_,
            Riscv::InstructionCacheConfiguration{
                p.find<std::uint64_t>("instruction_cache_bytes", 8192),
                p.find<std::uint32_t>("instruction_cache_line_bytes", 64),
                p.find<std::uint32_t>("instruction_cache_ways", 2),
                p.find<std::uint64_t>("instruction_cache_hit_cycles", 1)});
        if (p.find<bool>("instruction_cache_enabled", true)) instructionCache_ = std::move(cache);
        boot_ = Riscv::loadScratchpadBootImage(config_.elfPath, config_.scratchpadBase, capacity_);
        const auto file = p.find<std::string>("memory_file", "");
        backingFd_ = ::open(file.c_str(), O_RDWR | O_CLOEXEC);
        struct stat status{};
        if (backingFd_ < 0 || ::fstat(backingFd_, &status) ||
            status.st_size != static_cast<off_t>(capacity_))
            throw std::invalid_argument("memory_file must be a writable, capacity-sized shared SPM file");
        bridge_.create(0);
        bridge_.configureInstructionSegments(1);
        bridge_.configureLoadStoreQueue(loadStoreDepth_);
        bridge_.configureAnalogQueue(analogDepth_);
        void* mapped = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, MAP_SHARED, backingFd_, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("cannot map CPU load/store queue backing");
        backingBytes_ = static_cast<std::uint8_t*>(mapped);
        config_.syncBridgeFileDescriptor = bridge_.fileDescriptor();
        config_.scratchpadFileDescriptor = backingFd_;
        clock_ = registerTimeBase(Clock);
        wake_ = configureSelfLink("execute", clock_,
            new SST::Event::Handler<RiscvQemu, &RiscvQemu::step>(this));
        cacheWake_ = configureSelfLink("instruction_cache", clock_,
            new SST::Event::Handler<RiscvQemu, &RiscvQemu::cacheLookup>(this));
        memory_ = loadUserSubComponent<Memory>("qemu_memory", SST::ComponentInfo::SHARE_NONE,
            clock_, new Memory::Handler<RiscvQemu, &RiscvQemu::memoryResponse>(this));
        if (!memory_) throw std::invalid_argument("qemu_memory StandardMem interface is required");
        commit_ = configureLink("external_commit", clock_);
        if (!commit_) throw std::invalid_argument("SPM external_commit link is required");
        analog_ = configureLink("analog_commands", clock_,
            new SST::Event::Handler<RiscvQemu, &RiscvQemu::analogResponse>(this));
        if (const auto dir = CycleProfile::traceDirectory(); !dir.empty()) {
            taskTrace_.open(std::string(dir) + "/" + getName() + "-tasks.csv");
            if (!taskTrace_) throw std::runtime_error("cannot open CPU task trace");
            taskTrace_ << "event,cycle,task_id,execution_id,instructions,vector_instructions,"
                          "issue_cycles,read_bytes,write_bytes,fetch_bytes,memory_requests,"
                          "completed_requests,analog_commands,analog_read_bytes,analog_write_bytes,"
                          "instruction_bytes,icache_fetches,icache_hits,icache_misses,icache_fills,"
                          "icache_fill_bytes,icache_evictions,icache_invalidations,icache_stall_cycles,"
                          "vector_memory_beats,vector_read_bytes,vector_write_bytes,"
                          "lsq_enqueued,lsq_completed,lsq_stall_cycles\n";
            cacheTrace_.open(std::string(dir) + "/" + getName() + "-icache.csv");
            if (!cacheTrace_) throw std::runtime_error("cannot open instruction cache trace");
            cacheTrace_ << "event,cycle,address,bytes\n";
            memoryTrace_.open(std::string(dir) + "/" + getName() + "-memory.csv");
            if (!memoryTrace_) throw std::runtime_error("cannot open CPU memory trace");
            memoryTrace_ << "event,cycle,pc,address,bytes,write,vector\n";
            loadStoreTrace_.open(std::string(dir) + "/" + getName() + "-lsq.csv");
            if (!loadStoreTrace_) throw std::runtime_error("cannot open load/store queue trace");
            loadStoreTrace_ << "event,cycle,token,slot,pc,address,bytes,write,occupancy,reason\n";
            waitTrace_.open(std::string(dir) + "/" + getName() + "-waits.csv");
            if (!waitTrace_) throw std::runtime_error("cannot open CPU wait trace");
            waitTrace_ << "start_cycle,end_cycle,reason,stop_reason,pc\n";
            analogQueueTrace_.open(std::string(dir) + "/" + getName() + "-asq.csv");
            analogWaitTrace_.open(std::string(dir) + "/" + getName() + "-asq-waits.csv");
            if (!analogQueueTrace_ || !analogWaitTrace_)
                throw std::runtime_error("cannot open analog command queue traces");
            analogQueueTrace_ << "event,cycle,token,queue_token,slot,pc,operation,array,offset,count,register_mask,occupancy,active_bytes\n";
            analogWaitTrace_ << "start_cycle,end_cycle,reason,stop_reason,pc,mask,any\n";
        }
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    } catch (const std::exception& error) { fail(error.what()); }
}

RiscvQemu::~RiscvQemu() {
    process_.terminate();
    if (backingBytes_) ::munmap(backingBytes_, capacity_);
    if (backingFd_ >= 0) ::close(backingFd_);
}

void RiscvQemu::init(unsigned phase) { memory_->init(phase); }

void RiscvQemu::setup() {
    try {
        memory_->setup();
        // ELF initialization is untimed. PT_LOAD zero-fill includes BSS.
        Riscv::seedScratchpadBootImage(backingFd_, 0, boot_);
        process_.start(config_);
        wake_->send(0, new SST::Event);
    } catch (const std::exception& error) { fail(error.what()); }
}

void RiscvQemu::step(SST::Event* wake) {
    delete wake;
    try {
        if (!started_) { started_ = true; grant(); }
        else dispatch();
    } catch (const std::exception& error) { fail(error.what()); }
}

void RiscvQemu::grant() {
    fetchedInGrant_ = false;
    ledger_.beginGrant(bridge_.grant(budget_), budget_);
    ++grants_;
    capture();
}

void RiscvQemu::capture() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds_);
    while (true) {
        auto next = bridge_.waitForEvent(std::chrono::milliseconds(10));
        if (next) { event_ = std::move(next); break; }
        if (bridge_.protocolError()) throw std::runtime_error("QEMU synchronization protocol error");
        if (auto exit = process_.pollExit())
            throw std::runtime_error("QEMU exited before guest-exit synchronization: " + exit->describe());
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("QEMU rendezvous exceeded host_timeout_seconds");
    }
    ++stops_;
    if (!event_->memoryBatch.empty() || !event_->globalDMASubmitBatch.empty() ||
        !event_->analogSubmitBatch.empty())
        throw std::runtime_error("standalone CPU requires unbatched synchronous QEMU accesses");
    const Riscv::CpuExecutionLedger::Counts counts{
        event_->instructionsExecuted, event_->vectorInstructionsExecuted};
    ledger_.validateCaptured(event_->grantEpoch, counts);
    const bool boundary = event_->stopReason != MITTENS_SYNC_STOP_QUANTUM_END &&
                          event_->stopReason != MITTENS_SYNC_STOP_INSTRUCTION_FETCH;
    const auto charge = ledger_.accountTo(counts, boundary);
    // Cache lookup already paid for one issue cycle. Preserve the original
    // cache's credit reconciliation, including issue_width > 1 and traps.
    const auto credit = std::min(prepaidIssue_, charge.cycles.value);
    prepaidIssue_ -= credit;
    // The preceding resume has now performed the ordinary shared-memory access.
    releaseAccessRanges();
    wake_->send(charge.cycles.value - credit, new SST::Event);
}

void RiscvQemu::resume() {
    bridge_.resume(*event_);
    // Capture immediately at the same simulated time: QEMU performs the just
    // authorized shared-memory access before another SST event can run.
    capture();
}

void RiscvQemu::dispatch() {
    if (loadStoreBarrier()) return;
    if (analogQueueBarrier()) return;
    switch (event_->stopReason) {
    case MITTENS_SYNC_STOP_MEMORY_FENCE:
    case MITTENS_SYNC_STOP_INSTRUCTION_FENCE:
    case MITTENS_SYNC_STOP_TASK_START:
    case MITTENS_SYNC_STOP_TASK_FINISH:
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        if (!startedExecutions_.empty()) {
            // Arm only after this event's issue delay has elapsed. Completion
            // callbacks must not dispatch a newer event during its own delay,
            // cache lookup, memory access, or blocking analog transfer.
            executionDrainWaiting_ = true;
            return;
        }
        break;
    default: break;
    }
    switch (event_->stopReason) {
    case MITTENS_SYNC_STOP_QUANTUM_END:
        // In strict mode every executed instruction has a fetch rendezvous.
        // An unmapped access can trap into an unmapped vector before that
        // helper runs; QEMU then consumes quanta without any modeled fetch.
        if (!fetchedInGrant_)
            throw std::runtime_error("QEMU grant made no instruction-fetch progress in local SPM; check guest traps and addresses");
        grant(); break;
    case MITTENS_SYNC_STOP_INSTRUCTION_FETCH:
        fetchedInGrant_ = true;
        bridge_.approveInstructionSegment(1);
        if (instructionCache_) instructionFetch(); else access(true);
        break;
    case MITTENS_SYNC_STOP_MEMORY_ACCESS: access(false); break;
    case MITTENS_SYNC_STOP_LSQ_SUBMIT: enqueueLoadStore(); break;
    case MITTENS_SYNC_STOP_LSQ_WAIT: resume(); break;
    case MITTENS_SYNC_STOP_VECTOR_ANALOG: analogCommand(); break;
    case MITTENS_SYNC_STOP_ASQ_SUBMIT: enqueueAnalog(); break;
    case MITTENS_SYNC_STOP_ASQ_WAIT: resume(); break;
    case MITTENS_SYNC_STOP_MEMORY_FENCE: resume(); break;
    case MITTENS_SYNC_STOP_INSTRUCTION_FENCE:
        if (instructionCache_) {
            instructionCache_->invalidate();
            traceCache("invalidate", 0, 0);
        }
        resume(); break;
    case MITTENS_SYNC_STOP_TASK_START:
    case MITTENS_SYNC_STOP_TASK_FINISH: traceTask(); resume(); break;
    case MITTENS_SYNC_STOP_GUEST_EXIT: guestExit(); break;
    default:
        throw std::runtime_error("unsupported standalone CPU stop reason " +
                                 std::to_string(event_->stopReason));
    }
}

void RiscvQemu::traceTask() {
    // Snapshot after charging the marker's retired instructions. Logging adds
    // no simulated events or latency; guest marker instructions still execute.
    if (observationTaskPending_ && event_->stopReason == MITTENS_SYNC_STOP_TASK_START &&
        event_->taskId == observationStartTask_) {
        RecordComponentObservations = true;
        observationTaskPending_ = false;
    }
    if (!taskTrace_) return;
    if (analogQueueTrace_) analogQueueTrace_.flush();
    if (analogWaitTrace_) analogWaitTrace_.flush();
    const auto counts = ledger_.snapshot();
    const auto cache = instructionCache_ ? instructionCache_->statistics() : Riscv::InstructionCacheStatistics{};
    taskTrace_ << (event_->stopReason == MITTENS_SYNC_STOP_TASK_START ? "start" : "finish")
        << ',' << getCurrentSimTime(clock_) << ',' << event_->taskId << ',' << event_->executionId
        << ',' << counts.total.instructions << ',' << counts.total.vectors << ',' << counts.totalCycles
        << ',' << reads_ << ',' << writes_ << ',' << fetches_ << ',' << requests_ << ',' << completions_
        << ',' << analogCommands_ << ',' << analogReadBytes_ << ',' << analogWriteBytes_
        << ',' << instructionBytes_ << ',' << cache.fetches << ',' << cache.hits << ',' << cache.misses
        << ',' << cache.fills << ',' << cache.fillBytes << ',' << cache.evictions
        << ',' << cache.invalidations << ',' << cache.stallCycles
        << ',' << vectorMemoryBeats_ << ',' << vectorReadBytes_ << ',' << vectorWriteBytes_
        << ',' << lsqEnqueued_ << ',' << lsqCompleted_ << ',' << lsqStallCycles_ << '\n';
}

void RiscvQemu::traceCache(const char* event, std::uint64_t address, std::uint64_t bytes) {
    if (!RecordComponentObservations) return;
    if (cacheTrace_) cacheTrace_ << event << ',' << getCurrentSimTime(clock_) << ',' << address << ',' << bytes << '\n';
}

void RiscvQemu::instructionFetch() {
    const auto address = event_->memoryAddress;
    const auto bytes = event_->memorySize;
    const bool hit = instructionCache_->beginFetch(address, bytes);
    instructionBytes_ += bytes;
    cacheStart_ = getCurrentSimTime(clock_);
    traceCache(hit ? "hit" : "miss", address, bytes);
    const auto lineBytes = instructionCache_->configuration().lineBytes;
    cacheLine_ = (address - config_.scratchpadBase) & ~(std::uint64_t(lineBytes) - 1);
    cacheLastLine_ = (address - config_.scratchpadBase + bytes - 1) & ~(std::uint64_t(lineBytes) - 1);
    cacheWake_->send(instructionCache_->configuration().hitLatencyCycles, new SST::Event);
}

void RiscvQemu::cacheLookup(SST::Event* event) {
    delete event;
    try { advanceInstructionFetch(); }
    catch (const std::exception& error) { fail(error.what()); }
}

void RiscvQemu::advanceInstructionFetch() {
    const auto bytes = instructionCache_->configuration().lineBytes;
    while (cacheLine_ <= cacheLastLine_) {
        if (!instructionCache_->touch(cacheLine_)) {
            cacheFillPending_ = true;
            fetches_ += bytes;
            issueMemory(config_.scratchpadBase + cacheLine_, bytes, false);
            return;
        }
        cacheLine_ += bytes;
    }
    instructionCache_->accountStall(getCurrentSimTime(clock_) - cacheStart_ - 1);
    // Any unused preceding credit belongs to an instruction which did not
    // consume another issue cycle. It must not accumulate across fetches.
    prepaidIssue_ = 1;
    resume();
}

void RiscvQemu::releaseAccessRanges() {
    if (accessRanges_.empty()) return;
    auto* commit = new ExternalCommit;
    commit->ranges = std::move(accessRanges_);
    commit_->send(0, commit);
    accessRanges_.clear();
}

void RiscvQemu::access(bool fetch) {
    const auto address = event_->memoryAddress;
    const auto size = event_->memorySize;
    if (!size || !mittens_memory_contains_range(config_.scratchpadBase, capacity_, address, size))
        throw std::runtime_error("CPU instruction/data access is outside the local SPM");
    const bool write = !fetch && (event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_WRITE);
    const bool vector = !fetch && (event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_PREACCESS_VECTOR);
    if (vector) {
        if (!config_.riscvVectorEnabled || size > config_.riscvVectorLengthBits / 8 ||
            !(event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD))
            throw std::runtime_error("invalid pre-access RVV memory beat");
        ++vectorMemoryBeats_;
        (write ? vectorWriteBytes_ : vectorReadBytes_) += size;
    }
    if (fetch) { fetches_ += size; instructionBytes_ += size; }
    else if (write) writes_ += size;
    else reads_ += size;
    if (!fetch) traceMemory("issue");
    issueMemory(address, size, write);
}

void RiscvQemu::traceMemory(const char* kind) {
    if (!RecordComponentObservations) return;
    if (memoryTrace_) memoryTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << event_->memoryProgramCounter() << ',' << event_->memoryAddress << ',' << event_->memorySize
        << ',' << bool(event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_WRITE)
        << ',' << bool(event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_PREACCESS_VECTOR) << '\n';
}

void RiscvQemu::issueMemory(std::uint64_t address, std::uint64_t size, bool write) {
    if (!pending_.empty() || !accessRanges_.empty())
        throw std::runtime_error("new SPM access before the preceding access committed");
    // All fragments are admitted to the same banked controller. QEMU remains
    // paused until the last completion, including writes and split RVV beats.
    // Split only at transport boundaries. Each fragment retains its actual
    // byte range, so disjoint sub-line accesses can proceed independently.
    // Bank/channel arbitration determines parallel service.
    for (std::uint64_t offset = 0; offset < size;) {
        const auto local = address - config_.scratchpadBase + offset;
        const auto count = std::min<std::uint64_t>(size - offset, requestBytes_ - local % requestBytes_);
        accessRanges_.emplace_back(local, count);
        Memory::Request* request;
        if (write) {
            // No write value exists at this pre-access QEMU boundary. The
            // controller recognizes this explicit external_write_requestor
            // and times the write without overwriting the shared bytes.
            request = new Memory::Write(local, count, std::vector<std::uint8_t>(count));
        } else request = new Memory::Read(local, count);
        request->setNoncacheable();
        pending_.emplace(request->getID(), write);
        ++requests_;
        memory_->send(request);
        offset += count;
    }
}

void RiscvQemu::memoryResponse(Memory::Request* response) {
    try {
        if (loadStoreRequests_.count(response->getID())) {
            loadStoreResponse(response);
            return;
        }
        const auto found = pending_.find(response->getID());
        if (found == pending_.end() || response->getFail() ||
            (found->second ? !dynamic_cast<Memory::WriteResp*>(response) :
                             !dynamic_cast<Memory::ReadResp*>(response)))
            throw std::runtime_error("invalid SPM completion for QEMU request");
        pending_.erase(found);
        ++completions_;
        delete response;
        if (pending_.empty()) {
            if (cacheFillPending_) {
                // This was a real SST read, so the fill is complete now. Release
                // its byte ranges before another cache-line fill.
                releaseAccessRanges();
                instructionCache_->fill(cacheLine_);
                const auto bytes = instructionCache_->configuration().lineBytes;
                traceCache("fill", config_.scratchpadBase + cacheLine_, bytes);
                cacheLine_ += bytes;
                cacheFillPending_ = false;
                advanceInstructionFetch();
            } else {
                if (event_->stopReason == MITTENS_SYNC_STOP_MEMORY_ACCESS) traceMemory("ready");
                resume();
            }
        }
    } catch (const std::exception& error) { fail(error.what()); }
}

void RiscvQemu::analogCommand() {
    const auto transfer = bridge_.vectorAnalog(*event_);
    if (analogPending_ || transfer.status != UINT32_MAX)
        throw std::runtime_error("duplicate analog command");
    if (!analog_ || transfer.operation > 3 || transfer.array_id > UINT32_MAX ||
        transfer.element_count > sizeof(transfer.data) / 4 || transfer.reserved ||
        (transfer.operation == 2 && (transfer.element_count || transfer.element_offset))) {
        bridge_.completeVectorAnalog(*event_, 1); resume(); return;
    }
    auto* command = new ArrayCommand;
    command->operation = static_cast<Operation>(transfer.operation);
    command->array = transfer.array_id;
    command->token = event_->eventSequence;
    command->elementOffset = transfer.element_offset;
    command->elementCount = transfer.element_count;
    if (transfer.operation < 2)
        command->payload.assign(transfer.data, transfer.data + transfer.element_count * 4);
    analogPending_ = true; analogAccepted_ = false;
    analog_->send(command);
}

void RiscvQemu::analogResponse(SST::Event* event) {
    std::unique_ptr<SST::Event> owned(event);
    try {
        const auto* answer = dynamic_cast<ArrayCommand*>(event);
        if (!answer) throw std::runtime_error("invalid analog response type");
        if (answer->deferred) {
            queuedAnalogResponse(*answer);
            return;
        }
        const auto execution = startedExecutions_.find(answer->token);
        if (execution != startedExecutions_.end()) {
            // A started computation owns its input snapshot and must complete
            // successfully. Its reply cannot read or modify the current bridge
            // payload, which may now describe any other QEMU instruction.
            if (answer->array != execution->second || answer->operation != Operation::Execute ||
                answer->elementOffset || answer->elementCount || !answer->payload.empty() ||
                answer->status != CommandStatus::Complete)
                throw std::runtime_error("invalid completion of started analog computation");
            ++analogCommands_;
            startedExecutions_.erase(execution);
            if (executionDrainWaiting_ && startedExecutions_.empty()) {
                executionDrainWaiting_ = false;
                wake_->send(0, new SST::Event);
            }
            return;
        }
        if (!event_ || !analogPending_)
            throw std::runtime_error("analog response has no pending QEMU command");
        const auto transfer = bridge_.vectorAnalog(*event_);
        if (answer->token != event_->eventSequence ||
            answer->array != transfer.array_id || unsigned(answer->operation) != transfer.operation ||
            answer->elementOffset != transfer.element_offset || answer->elementCount != transfer.element_count)
            throw std::runtime_error("invalid analog completion for QEMU");
        if (answer->status == CommandStatus::Accepted) {
            if (analogAccepted_ || !answer->payload.empty())
                throw std::runtime_error("invalid or duplicate analog acceptance");
            analogAccepted_ = true; return;
        }
        if (answer->status == CommandStatus::Started) {
            if (!arrayPipelineEnabled_ || transfer.operation != 2 || !analogAccepted_ ||
                !answer->payload.empty())
                throw std::runtime_error("invalid analog computation start");
            startedExecutions_.emplace(answer->token, answer->array);
            // In pipeline mode mvm's zero status means validated and started.
            // Program/Load/Store still complete before their helpers return.
            bridge_.completeVectorAnalog(*event_, 0);
            analogPending_ = false;
            resume();
            return;
        }
        if (answer->status == CommandStatus::Busy) {
            if (analogAccepted_) throw std::runtime_error("accepted analog command returned busy");
            analogPending_ = false; wake_->send(1, new SST::Event); return;
        }
        const bool success = answer->status == CommandStatus::Complete;
        if (success && arrayPipelineEnabled_ && transfer.operation == 2)
            throw std::runtime_error("pipelined analog computation completed without a start response");
        if ((!success && answer->status != CommandStatus::Error) ||
            (success && (!analogAccepted_ || answer->payload.size() !=
                         (transfer.operation == 3 ? transfer.element_count * 4 : 0))))
            throw std::runtime_error("malformed analog completion payload/status");
        if (success) {
            ++analogCommands_;
            if (transfer.operation == 3) analogReadBytes_ += answer->payload.size();
            else analogWriteBytes_ += transfer.element_count * 4;
        }
        bridge_.completeVectorAnalog(*event_, success ? 0 : 1, answer->payload);
        analogPending_ = false; resume();
    } catch (const std::exception& error) { fail(error.what()); }
}

void RiscvQemu::guestExit() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds_);
    while (true) {
        if (auto exit = process_.pollExit()) {
            if (!exit->success()) throw std::runtime_error("guest failed: " + exit->describe());
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("QEMU did not exit after guest-exit synchronization");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!pending_.empty() || !loadStoreEntries_.empty() || !loadStoreRequests_.empty() || lsqWaiting_ ||
        analogPending_ || !startedExecutions_.empty() || executionDrainWaiting_ ||
        !analogEntries_.empty() || asqWaiting_ || analogAdmissionToken_)
        throw std::runtime_error("guest exited with outstanding requests");
    done_ = true;
    endCycle_ = getCurrentSimTime(clock_);
    primaryComponentOKToEndSim();
}

void RiscvQemu::finish() {
    if (!done_) fail("simulation ended before the RISC-V guest completed");
    memory_->finish();
    if (taskTrace_) taskTrace_.flush();
    if (cacheTrace_) cacheTrace_.flush();
    if (memoryTrace_) memoryTrace_.flush();
    if (loadStoreTrace_) loadStoreTrace_.flush();
    if (analogQueueTrace_) analogQueueTrace_.flush();
    if (analogWaitTrace_) analogWaitTrace_.flush();
    const auto counts = ledger_.snapshot();
    const auto cache = instructionCache_ ? instructionCache_->statistics() : Riscv::InstructionCacheStatistics{};
    std::cout << "RISCV_STATS {\"component\":" << std::quoted(getName())
        << ",\"instructions\":" << counts.total.instructions
        << ",\"vector_instructions\":" << counts.total.vectors
        << ",\"issue_cycles\":" << counts.totalCycles
        << ",\"read_bytes\":" << reads_ << ",\"write_bytes\":" << writes_
        << ",\"fetch_bytes\":" << fetches_ << ",\"memory_requests\":" << requests_
        << ",\"completed_requests\":" << completions_ << ",\"grants\":" << grants_
        << ",\"vector_memory_beats\":" << vectorMemoryBeats_
        << ",\"vector_read_bytes\":" << vectorReadBytes_ << ",\"vector_write_bytes\":" << vectorWriteBytes_
        << ",\"stops\":" << stops_ << ",\"end_cycle\":" << endCycle_
        << ",\"analog_commands\":" << analogCommands_ << ",\"analog_read_bytes\":" << analogReadBytes_
        << ",\"analog_write_bytes\":" << analogWriteBytes_
        << ",\"instruction_bytes\":" << instructionBytes_
        << ",\"icache_fetches\":" << cache.fetches << ",\"icache_hits\":" << cache.hits
        << ",\"icache_misses\":" << cache.misses << ",\"icache_fills\":" << cache.fills
        << ",\"icache_fill_bytes\":" << cache.fillBytes << ",\"icache_evictions\":" << cache.evictions
        << ",\"icache_invalidations\":" << cache.invalidations
        << ",\"icache_stall_cycles\":" << cache.stallCycles
        << ",\"load_store_queue_depth\":" << loadStoreDepth_
        << ",\"lsq_enqueued\":" << lsqEnqueued_ << ",\"lsq_completed\":" << lsqCompleted_
        << ",\"lsq_peak_occupancy\":" << lsqPeak_
        << ",\"lsq_register_stalls\":" << lsqRegisterStalls_
        << ",\"lsq_full_stalls\":" << lsqFullStalls_ << ",\"lsq_drain_stalls\":" << lsqDrainStalls_
        << ",\"lsq_stall_cycles\":" << lsqStallCycles_
        << ",\"analog_command_queue_depth\":" << analogDepth_
        << ",\"analog_command_queue_bytes\":" << analogByteCapacity_
        << ",\"asq_enqueued\":" << asqEnqueued_ << ",\"asq_completed\":" << asqCompleted_
        << ",\"asq_peak_occupancy\":" << asqPeak_ << ",\"asq_peak_bytes\":" << asqPeakBytes_
        << ",\"asq_busy\":" << asqBusy_ << ",\"asq_guaranteed\":" << asqGuaranteed_
        << ",\"asq_fallback\":" << asqFallback_ << ",\"asq_stall_cycles\":" << asqStallCycles_ << "}\n";
}

void RiscvQemu::emergencyShutdown() { process_.terminate(); }

[[noreturn]] void RiscvQemu::fail(const std::string& message) {
    process_.terminate();
    fatal(CALL_INFO, -1, "RISC-V QEMU: %s\n", message.c_str());
    std::abort();
}
}
