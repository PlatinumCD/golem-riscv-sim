#include <sst/core/sst_config.h>
#include "riscvQemu.h"
#include "externalCommit.h"
#include "../observations.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace TileComponents {

bool RiscvQemu::loadStoreWaitSatisfied(std::uint64_t mask, bool any) const {
    std::uint64_t active = 0;
    for (const auto& [token, entry] : loadStoreEntries_)
        active |= UINT64_C(1) << entry.slot;
    return any ? (active & mask) != mask : (active & mask) == 0;
}

bool RiscvQemu::loadStoreBarrier() {
    if (loadStoreDepth_ == 1) {
        if (event_->stopReason == MITTENS_SYNC_STOP_LSQ_SUBMIT ||
            event_->stopReason == MITTENS_SYNC_STOP_LSQ_WAIT)
            throw std::runtime_error("deferred memory event with the blocking CPU configuration");
        return false;
    }
    std::uint64_t mask = 0;
    bool any = false;
    const char* reason = "drain";
    switch (event_->stopReason) {
    case MITTENS_SYNC_STOP_INSTRUCTION_FETCH:
    case MITTENS_SYNC_STOP_VECTOR_ANALOG:
        // QEMU checks analog source/destination groups before executing its
        // blocking helper. Retain that dependency mask at command dispatch;
        // unrelated memory may progress during the timed array transfer.
        mask = event_->loadStoreWaitMask;
        if (event_->loadStoreWaitReason > MITTENS_SYNC_LSQ_WAIT_DRAIN)
            throw std::runtime_error("invalid load/store fetch dependency reason");
        reason = event_->loadStoreWaitReason == MITTENS_SYNC_LSQ_WAIT_REGISTER ? "register" : "drain";
        break;
    case MITTENS_SYNC_STOP_LSQ_WAIT:
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
    case MITTENS_SYNC_STOP_LSQ_SUBMIT:
        if (loadStoreEntries_.size() >= loadStoreDepth_)
            throw std::runtime_error("QEMU submitted beyond the load/store queue capacity");
        break;
    default: break;
    }
    if (!mask || loadStoreWaitSatisfied(mask, any)) return false;
    if (lsqWaiting_) throw std::logic_error("duplicate load/store wait dispatch");
    lsqWaiting_ = true;
    lsqWaitAny_ = any;
    lsqWaitMask_ = mask;
    lsqWaitStart_ = getCurrentSimTime(clock_);
    lsqWaitReason_ = reason;
    lsqWaitStopReason_ = event_->stopReason;
    lsqWaitPc_ = event_->stopReason == MITTENS_SYNC_STOP_INSTRUCTION_FETCH ?
        event_->memoryAddress : event_->memoryProgramCounter();
    if (std::strcmp(reason, "register") == 0) ++lsqRegisterStalls_;
    else if (any) ++lsqFullStalls_;
    else ++lsqDrainStalls_;
    traceLoadStore("stall", nullptr, reason);
    return true;
}

void RiscvQemu::traceLoadStore(const char* kind, const LoadStoreEntry* entry, const char* reason) {
    if (!RecordComponentObservations) return;
    if (!loadStoreTrace_) return;
    loadStoreTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << (entry ? entry->token : 0) << ',' << (entry ? entry->slot : 0) << ','
        << (entry ? entry->pc : 0) << ',' << (entry ? entry->address : 0) << ','
        << (entry ? entry->bytes : 0) << ',' << (entry ? entry->write : false) << ','
        << loadStoreEntries_.size() << ',' << reason << '\n';
}

void RiscvQemu::traceLoadStoreMemory(const char* kind, const LoadStoreEntry& entry) {
    if (!RecordComponentObservations) return;
    if (memoryTrace_) memoryTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << entry.pc << ',' << entry.address << ',' << entry.bytes << ',' << entry.write << ",1\n";
}

void RiscvQemu::enqueueLoadStore() {
    const auto slotIndex = event_->taskId;
    if (slotIndex >= loadStoreDepth_ || !pending_.empty() || !accessRanges_.empty())
        throw std::runtime_error("invalid or overlapping load/store queue submission");
    auto& slot = bridge_.loadStoreQueue().slots[slotIndex];
    const auto size = slot.size;
    if (mittens_sync_load_acquire(&slot.state) != MITTENS_SYNC_LSQ_SUBMITTED ||
        slot.reserved || slot.token <= lastLoadStoreToken_ || slot.write > 1 ||
        !size || size > config_.riscvVectorLengthBits / 8 || size > MITTENS_SYNC_LSQ_BYTES ||
        (slot.element_bytes != 1 && slot.element_bytes != 2 && slot.element_bytes != 4 && slot.element_bytes != 8) ||
        slot.address % slot.element_bytes || size % slot.element_bytes ||
        !mittens_memory_contains_range(config_.scratchpadBase, capacity_, slot.address, size) ||
        size > 4096 - slot.address % 4096 ||
        (slot.write ? slot.destination != UINT32_MAX : slot.destination >= 32))
        throw std::runtime_error("invalid deferred vector memory descriptor");
    for (const auto& [token, entry] : loadStoreEntries_)
        if (entry.slot == slotIndex) throw std::runtime_error("load/store queue slot reused before completion");
    lastLoadStoreToken_ = slot.token;
    LoadStoreEntry entry{slot.token, slot.address, slot.program_counter, slotIndex,
                         size, bool(slot.write), false, 0, {}, {}};
    entry.data.resize(size);
    if (entry.write) std::copy_n(slot.data, size, entry.data.begin());
    auto [position, inserted] = loadStoreEntries_.emplace(entry.token, std::move(entry));
    if (!inserted) throw std::runtime_error("duplicate load/store token");
    auto& active = position->second;
    ++lsqEnqueued_;
    lsqPeak_ = std::max<std::uint64_t>(lsqPeak_, loadStoreEntries_.size());
    ++vectorMemoryBeats_;
    (active.write ? vectorWriteBytes_ : vectorReadBytes_) += size;
    (active.write ? writes_ : reads_) += size;
    mittens_sync_store_release(&slot.state, MITTENS_SYNC_LSQ_INFLIGHT);
    traceLoadStore("enqueue", &active);
    traceLoadStore("issue", &active);
    traceLoadStoreMemory("issue", active);
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
        loadStoreRequests_.emplace(request->getID(), active.token);
        ++active.pending;
        ++requests_;
        memory_->send(request);
        offset += count;
    }
    // Only admission resumes QEMU. Its architectural load destination remains
    // unavailable until completion, and stores no longer refer to live vregs.
    resume();
}

void RiscvQemu::loadStoreResponse(Memory::Request* response) {
    const auto request = loadStoreRequests_.find(response->getID());
    if (request == loadStoreRequests_.end()) throw std::runtime_error("unknown load/store response");
    auto& entry = loadStoreEntries_.at(request->second);
    if (response->getFail() || (entry.write ? !dynamic_cast<Memory::WriteResp*>(response) :
                                           !dynamic_cast<Memory::ReadResp*>(response)))
        throw std::runtime_error("invalid deferred load/store response");
    loadStoreRequests_.erase(request);
    ++completions_;
    delete response;
    if (!entry.pending || entry.serviced) throw std::runtime_error("duplicate load/store completion");
    if (--entry.pending == 0) {
        entry.serviced = true;
        // All the request's byte ranges are still retained by the controller.
        // This is the timed value snapshot, never a later reread by QEMU.
        if (!entry.write)
            std::copy_n(backingBytes_ + entry.address - config_.scratchpadBase, entry.bytes, entry.data.begin());
        traceLoadStore("service_complete", &entry);
        retireLoadStores();
    }
}

void RiscvQemu::retireLoadStores() {
    // This is an in-order, non-speculative vector LSU. Responses may arrive
    // independently, but architectural publication and store visibility retire
    // in issue order. The scratchpad already serializes overlapping byte ranges.
    while (!loadStoreEntries_.empty() && loadStoreEntries_.begin()->second.serviced) {
        auto position = loadStoreEntries_.begin();
        auto& entry = position->second;
        auto& slot = bridge_.loadStoreQueue().slots[entry.slot];
        if (slot.token != entry.token || mittens_sync_load_acquire(&slot.state) != MITTENS_SYNC_LSQ_INFLIGHT)
            throw std::runtime_error("deferred memory slot changed while outstanding");
        if (entry.write)
            std::copy(entry.data.begin(), entry.data.end(), backingBytes_ + entry.address - config_.scratchpadBase);
        else std::copy(entry.data.begin(), entry.data.end(), slot.data);
        auto* commit = new ExternalCommit;
        commit->ranges = entry.ranges;
        commit_->send(0, commit);
        mittens_sync_store_release(&slot.state, MITTENS_SYNC_LSQ_COMPLETE);
        traceLoadStoreMemory("ready", entry);
        const auto retired = entry;
        loadStoreEntries_.erase(position);
        ++lsqCompleted_;
        traceLoadStore("complete", &retired);
    }
    // Only a dispatch which already consumed its instruction-issue delay may
    // be restarted. Other completions must not bypass a scheduled CPU/cache wake.
    if (lsqWaiting_ && loadStoreWaitSatisfied(lsqWaitMask_, lsqWaitAny_)) {
        const auto end = getCurrentSimTime(clock_);
        lsqStallCycles_ += end - lsqWaitStart_;
        // Observation only: record the exact existing wait interval before its
        // ordinary wake. This adds no simulated event or synchronization step.
        if (RecordComponentObservations && waitTrace_) waitTrace_ << lsqWaitStart_ << ',' << end << ','
            << lsqWaitReason_ << ',' << lsqWaitStopReason_ << ',' << lsqWaitPc_ << '\n';
        lsqWaiting_ = false;
        wake_->send(0, new SST::Event);
    }
}
}
