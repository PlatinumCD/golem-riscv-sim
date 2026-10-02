#include <sst/core/sst_config.h>
#include "riscvQemu.h"
#include "externalCommit.h"
#include "../observations.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace TileComponents {

bool RiscvQemu::scalarWaitSatisfied(std::uint64_t mask, bool any) const {
    std::uint64_t active = 0;
    for (const auto& [token, entry] : scalarEntries_)
        active |= UINT64_C(1) << entry.slot;
    return any ? (active & mask) != mask : (active & mask) == 0;
}

bool RiscvQemu::scalarBarrier() {
    if (scalarDepth_ == 0) {
        if (event_->stopReason == MITTENS_SYNC_STOP_SLQ_SUBMIT ||
            event_->stopReason == MITTENS_SYNC_STOP_SLQ_WAIT)
            throw std::runtime_error("deferred memory event with the blocking CPU configuration");
        return false;
    }
    std::uint64_t mask = 0;
    bool any = false;
    const char* reason = "drain";
    switch (event_->stopReason) {
    case MITTENS_SYNC_STOP_NETWORK: {
        const auto command = bridge_.networkCommand(*event_);
        if (command.operation == 0) {
            // Release ordering for descriptors and source data, without
            // draining unrelated vector loads or analog operations.
            for (const auto& [token, entry] : scalarEntries_)
                if (entry.write) mask |= UINT64_C(1) << entry.slot;
        }
        break;
    }
    case MITTENS_SYNC_STOP_INSTRUCTION_FETCH:
    case MITTENS_SYNC_STOP_VECTOR_ANALOG:
        // Retain the scalar register dependency mask at command dispatch;
        // unrelated accesses may progress during the timed array transfer.
        mask = event_->scalarWaitMask;
        if (event_->scalarWaitReason > MITTENS_SYNC_LSQ_WAIT_DRAIN)
            throw std::runtime_error("invalid load/store fetch dependency reason");
        reason = event_->scalarWaitReason == MITTENS_SYNC_LSQ_WAIT_REGISTER ? "register" : "drain";
        break;
    case MITTENS_SYNC_STOP_SLQ_WAIT:
        mask = event_->memoryAddress;
        if (event_->memoryFlags > 1 || !mask)
            throw std::runtime_error("invalid load/store wait mask or mode");
        any = event_->memoryFlags == 1;
        reason = any ? "full" : "drain";
        break;
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
    case MITTENS_SYNC_STOP_MEMORY_FENCE:
    case MITTENS_SYNC_STOP_INSTRUCTION_FENCE:
    case MITTENS_SYNC_STOP_TASK_START:
    case MITTENS_SYNC_STOP_TASK_FINISH:
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        mask = UINT64_MAX;
        break;
    case MITTENS_SYNC_STOP_SLQ_SUBMIT:
        if (scalarEntries_.size() >= scalarDepth_)
            throw std::runtime_error("QEMU submitted beyond the load/store queue capacity");
        break;
    default: break;
    }
    if (!mask || scalarWaitSatisfied(mask, any)) return false;
    if (slqWaiting_) throw std::logic_error("duplicate load/store wait dispatch");
    slqWaiting_ = true;
    slqWaitAny_ = any;
    slqWaitMask_ = mask;
    slqWaitStart_ = getCurrentSimTime(clock_);
    slqWaitReason_ = reason;
    slqWaitStopReason_ = event_->stopReason;
    slqWaitPc_ = event_->stopReason == MITTENS_SYNC_STOP_INSTRUCTION_FETCH ?
        event_->memoryAddress : event_->memoryProgramCounter();
    if (std::strcmp(reason, "register") == 0) ++slqRegisterStalls_;
    else if (any) ++slqFullStalls_;
    else ++slqDrainStalls_;
    traceScalar("stall", nullptr, reason);
    return true;
}

void RiscvQemu::traceScalar(const char* kind, const LoadStoreEntry* entry, const char* reason) {
    if (!RecordComponentObservations) return;
    if (!scalarTrace_) return;
    scalarTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << (entry ? entry->token : 0) << ',' << (entry ? entry->slot : 0) << ','
        << (entry ? entry->pc : 0) << ',' << (entry ? entry->address : 0) << ','
        << (entry ? entry->bytes : 0) << ',' << (entry ? entry->write : false) << ','
        << scalarEntries_.size() << ',' << reason << '\n';
}

void RiscvQemu::traceScalarMemory(const char* kind, const LoadStoreEntry& entry) {
    if (!RecordComponentObservations) return;
    if (memoryTrace_) memoryTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << entry.pc << ',' << entry.address << ',' << entry.bytes << ',' << entry.write << ",0\n";
}

void RiscvQemu::enqueueScalar() {
    const auto slotIndex = event_->taskId;
    if (slotIndex >= scalarDepth_ || !pending_.empty() || !accessRanges_.empty())
        throw std::runtime_error("invalid or overlapping load/store queue submission");
    auto& slot = bridge_.scalarQueue().slots[slotIndex];
    const auto size = slot.size;
    if (mittens_sync_load_acquire(&slot.state) != MITTENS_SYNC_LSQ_SUBMITTED ||
        slot.reserved || slot.token <= lastScalarToken_ || slot.write > 1 ||
        !size || size > 8 ||
        (slot.element_bytes != 1 && slot.element_bytes != 2 && slot.element_bytes != 4 && slot.element_bytes != 8) ||
        slot.address % slot.element_bytes || size != slot.element_bytes ||
        !mittens_memory_contains_range(config_.scratchpadBase, capacity_, slot.address, size) ||
        size > 4096 - slot.address % 4096 ||
        (slot.write ? slot.destination != UINT32_MAX : slot.destination >= 64))
        throw std::runtime_error("invalid deferred scalar memory descriptor");
    for (const auto& [token, entry] : scalarEntries_)
        if (entry.slot == slotIndex) throw std::runtime_error("load/store queue slot reused before completion");
    lastScalarToken_ = slot.token;
    LoadStoreEntry entry{slot.token, slot.address, slot.program_counter, slotIndex,
                         size, bool(slot.write), false, 0, {}, {}};
    entry.data.resize(size);
    if (entry.write) std::copy_n(slot.data, size, entry.data.begin());
    auto [position, inserted] = scalarEntries_.emplace(entry.token, std::move(entry));
    if (!inserted) throw std::runtime_error("duplicate load/store token");
    auto& active = position->second;
    ++slqEnqueued_;
    slqPeak_ = std::max<std::uint64_t>(slqPeak_, scalarEntries_.size());
    (active.write ? writes_ : reads_) += size;
    mittens_sync_store_release(&slot.state, MITTENS_SYNC_LSQ_INFLIGHT);
    traceScalar("enqueue", &active);
    traceScalar("issue", &active);
    traceScalarMemory("issue", active);
    for (std::uint64_t offset = 0; offset < size;) {
        const auto local = active.address - config_.scratchpadBase + offset;
        const auto count = std::min<std::uint64_t>(size - offset, requestBytes_ - local % requestBytes_);
        active.ranges.emplace_back(local, count);
        Memory::Request* request;
        // The controller's external requestor policy holds exact byte ranges and
        // times writes without changing mmap bytes. This queue commits captured
        // store data only at ordered retirement below.
        if (active.write) request = new Memory::Write(local, count,
            std::vector<std::uint8_t>(active.data.begin() + offset, active.data.begin() + offset + count));
        else request = new Memory::Read(local, count);
        request->setNoncacheable();
        scalarRequests_.emplace(request->getID(), active.token);
        ++active.pending;
        ++requests_;
        memory_->send(request);
        offset += count;
    }
    // Only admission resumes QEMU. Its architectural load destination remains
    // unavailable until completion; stores retain their captured scalar value.
    resume();
}

void RiscvQemu::scalarResponse(Memory::Request* response) {
    const auto request = scalarRequests_.find(response->getID());
    if (request == scalarRequests_.end()) throw std::runtime_error("unknown load/store response");
    auto& entry = scalarEntries_.at(request->second);
    if (response->getFail() || (entry.write ? !dynamic_cast<Memory::WriteResp*>(response) :
                                           !dynamic_cast<Memory::ReadResp*>(response)))
        throw std::runtime_error("invalid deferred load/store response");
    scalarRequests_.erase(request);
    ++completions_;
    delete response;
    if (!entry.pending || entry.serviced) throw std::runtime_error("duplicate load/store completion");
    if (--entry.pending == 0) {
        entry.serviced = true;
        // All the request's byte ranges are still retained by the controller.
        // This is the timed value snapshot, never a later reread by QEMU.
        if (!entry.write)
            std::copy_n(backingBytes_ + entry.address - config_.scratchpadBase, entry.bytes, entry.data.begin());
        traceScalar("service_complete", &entry);
        retireScalars();
    }
}

void RiscvQemu::retireScalars() {
    // This is an in-order, non-speculative scalar LSU. Responses may arrive
    // independently, but architectural publication and store visibility retire
    // in issue order. The scratchpad already serializes overlapping byte ranges.
    while (!scalarEntries_.empty() && scalarEntries_.begin()->second.serviced) {
        auto position = scalarEntries_.begin();
        auto& entry = position->second;
        auto& slot = bridge_.scalarQueue().slots[entry.slot];
        if (slot.token != entry.token || mittens_sync_load_acquire(&slot.state) != MITTENS_SYNC_LSQ_INFLIGHT)
            throw std::runtime_error("deferred memory slot changed while outstanding");
        if (entry.write)
            std::copy(entry.data.begin(), entry.data.end(), backingBytes_ + entry.address - config_.scratchpadBase);
        else std::copy(entry.data.begin(), entry.data.end(), slot.data);
        auto* commit = new ExternalCommit;
        commit->ranges = entry.ranges;
        commit_->send(0, commit);
        mittens_sync_store_release(&slot.state, MITTENS_SYNC_LSQ_COMPLETE);
        traceScalarMemory("ready", entry);
        const auto retired = entry;
        scalarEntries_.erase(position);
        ++slqCompleted_;
        traceScalar("complete", &retired);
    }
    // Only a dispatch which already consumed its instruction-issue delay may
    // be restarted. Other completions must not bypass a scheduled CPU/cache wake.
    if (slqWaiting_ && scalarWaitSatisfied(slqWaitMask_, slqWaitAny_)) {
        const auto end = getCurrentSimTime(clock_);
        slqStallCycles_ += end - slqWaitStart_;
        // Observation only: record the exact existing wait interval before its
        // ordinary wake. This adds no simulated event or synchronization step.
        if (RecordComponentObservations && scalarWaitTrace_) scalarWaitTrace_ << slqWaitStart_ << ',' << end << ','
            << slqWaitReason_ << ',' << slqWaitStopReason_ << ',' << slqWaitPc_ << '\n';
        slqWaiting_ = false;
        wake_->send(0, new SST::Event);
    }
}
}
