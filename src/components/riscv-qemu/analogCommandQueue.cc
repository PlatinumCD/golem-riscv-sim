#include <sst/core/sst_config.h>
#include "riscvQemu.h"
#include "../observations.h"
#include <algorithm>
#include <stdexcept>

namespace TileComponents {

bool RiscvQemu::analogQueueWaitSatisfied(std::uint64_t mask, bool any, std::uint32_t reason) const {
    std::uint64_t active = 0;
    for (const auto& [token, entry] : analogEntries_) {
        // Input register hazards release at timed consumption, while drains
        // and queue capacity wait for completion (including programming delay).
        if (reason == MITTENS_SYNC_ASQ_WAIT_REGISTER &&
            entry.operation != Operation::Store && entry.captured) continue;
        active |= UINT64_C(1) << entry.slot;
    }
    return any ? (active & mask) != mask : (active & mask) == 0;
}

bool RiscvQemu::analogQueueBarrier() {
    if (!analogDepth_) {
        if (event_->stopReason == MITTENS_SYNC_STOP_ASQ_SUBMIT ||
            event_->stopReason == MITTENS_SYNC_STOP_ASQ_WAIT)
            throw std::runtime_error("analog queue event while disabled");
        return false;
    }
    std::uint64_t mask = 0;
    bool any = false;
    auto reason = std::uint32_t(MITTENS_SYNC_ASQ_WAIT_DRAIN);
    switch (event_->stopReason) {
    case MITTENS_SYNC_STOP_INSTRUCTION_FETCH:
    case MITTENS_SYNC_STOP_VECTOR_ANALOG:
        mask = event_->analogWaitMask;
        reason = event_->analogWaitReason;
        if (reason > MITTENS_SYNC_ASQ_WAIT_FULL)
            throw std::runtime_error("invalid analog dependency wait reason");
        break;
    case MITTENS_SYNC_STOP_ASQ_WAIT:
        mask = event_->memoryAddress;
        if (event_->memoryFlags > 1 || !mask || mask >> analogDepth_)
            throw std::runtime_error("invalid analog queue wait mask or mode");
        any = event_->memoryFlags == 1;
        reason = any ? MITTENS_SYNC_ASQ_WAIT_FULL : MITTENS_SYNC_ASQ_WAIT_DRAIN;
        break;
    case MITTENS_SYNC_STOP_ASQ_SUBMIT: {
        if (event_->taskId >= analogDepth_)
            throw std::runtime_error("invalid analog queue submission slot");
        const auto& slot = bridge_.analogQueue().slots[event_->taskId];
        if (slot.element_count > MITTENS_SYNC_ASQ_BYTES / 4)
            throw std::runtime_error("oversized analog queue payload");
        if (!analogAdmissionToken_ && analogActiveBytes_ + slot.element_count * 4 > analogByteCapacity_) {
            for (const auto& [token, entry] : analogEntries_) mask |= UINT64_C(1) << entry.slot;
            if (!mask) throw std::logic_error("analog payload exceeds empty queue capacity");
            any = true;
            reason = MITTENS_SYNC_ASQ_WAIT_FULL;
        }
        break;
    }
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
        // With LSQ depth one, vector beats still block for SPM timing but
        // their register dependencies were checked at the instruction fetch.
        // Preserve overlap with unrelated analog work, including source
        // reuse after Captured while a programming delay remains outstanding.
        if (event_->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_PREACCESS_VECTOR) {
            mask = event_->analogWaitMask;
            reason = event_->analogWaitReason;
        } else mask = UINT64_MAX;
        break;
    case MITTENS_SYNC_STOP_MEMORY_FENCE:
    case MITTENS_SYNC_STOP_INSTRUCTION_FENCE:
    case MITTENS_SYNC_STOP_TASK_START:
    case MITTENS_SYNC_STOP_TASK_FINISH:
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        mask = UINT64_MAX;
        break;
    default: break;
    }
    if (!mask || analogQueueWaitSatisfied(mask, any, reason)) return false;
    if (asqWaiting_) throw std::logic_error("duplicate analog queue wait dispatch");
    asqWaiting_ = true;
    asqWaitMask_ = mask;
    asqWaitAny_ = any;
    asqWaitReason_ = reason;
    asqWaitStart_ = getCurrentSimTime(clock_);
    asqWaitStop_ = event_->stopReason;
    asqWaitPc_ = event_->stopReason == MITTENS_SYNC_STOP_INSTRUCTION_FETCH ?
        event_->memoryAddress : event_->memoryProgramCounter();
    traceAnalogQueue("stall");
    return true;
}

void RiscvQemu::traceAnalogQueue(const char* kind, const AnalogEntry* entry) {
    if (!RecordComponentObservations || !analogQueueTrace_) return;
    analogQueueTrace_ << kind << ',' << getCurrentSimTime(clock_) << ','
        << (entry ? entry->token : 0) << ',' << (entry ? entry->queueToken : 0) << ','
        << (entry ? entry->slot : 0) << ',' << (entry ? entry->pc : 0) << ','
        << (entry ? unsigned(entry->operation) : 0) << ',' << (entry ? entry->array : 0) << ','
        << (entry ? entry->offset : 0) << ',' << (entry ? entry->count : 0) << ','
        << (entry ? entry->registers : 0) << ',' << analogEntries_.size() << ',' << analogActiveBytes_ << '\n';
}

void RiscvQemu::enqueueAnalog() {
    const auto index = event_->taskId;
    if (index >= analogDepth_ || analogPending_ || !pending_.empty() || !accessRanges_.empty())
        throw std::runtime_error("invalid or overlapping analog queue admission");
    auto& slot = bridge_.analogQueue().slots[index];
    if (mittens_sync_load_acquire(&slot.state) != MITTENS_SYNC_ASQ_SUBMITTED)
        throw std::runtime_error("analog queue submission in invalid state");
    const auto token = event_->eventSequence;
    if (!analogAdmissionToken_) {
        if (slot.reserved || slot.token <= lastAnalogQueueToken_ || slot.operation > 3 ||
            slot.operation == 2 || slot.vector_register >= 32 ||
            !slot.register_mask || !(slot.register_mask & (UINT32_C(1) << slot.vector_register)) ||
            slot.element_count > MITTENS_SYNC_ASQ_BYTES / 4 ||
            analogEntries_.size() >= analogDepth_ ||
            analogActiveBytes_ + slot.element_count * 4 > analogByteCapacity_)
            throw std::runtime_error("invalid analog queue descriptor or capacity");
        for (const auto& [oldToken, entry] : analogEntries_)
            if (entry.slot == index) throw std::runtime_error("analog queue slot reused before completion");
        lastAnalogQueueToken_ = slot.token;
        // The array selector is a guest operand, not a bridge invariant.
        // Preserve the blocking helper's precise architectural rejection.
        if (slot.array_id > UINT32_MAX) {
            slot.status = 1;
            mittens_sync_store_release(&slot.state, MITTENS_SYNC_ASQ_ERROR);
            resume();
            return;
        }
        AnalogEntry entry{token, slot.token, slot.program_counter, slot.element_offset,
                          index, std::uint32_t(slot.array_id), slot.element_count, slot.register_mask,
                          static_cast<Operation>(slot.operation), {}};
        if (slot.operation < 2) entry.data.assign(slot.data, slot.data + slot.element_count * 4);
        const auto inserted = analogEntries_.emplace(token, std::move(entry));
        if (!inserted.second) throw std::runtime_error("duplicate analog queue token");
        analogAdmissionToken_ = token;
        analogActiveBytes_ += slot.element_count * 4;
        ++asqEnqueued_;
        asqPeak_ = std::max<std::uint64_t>(asqPeak_, analogEntries_.size());
        asqPeakBytes_ = std::max(asqPeakBytes_, analogActiveBytes_);
        traceAnalogQueue("enqueue", &inserted.first->second);
    }
    if (analogAdmissionToken_ != token)
        throw std::logic_error("analog admission retry does not own current CPU event");
    const auto& entry = analogEntries_.at(token);
    if (entry.accepted || entry.slot != index || entry.queueToken != slot.token)
        throw std::logic_error("invalid analog admission retry");
    if (!analog_) {
        ArrayCommand rejected;
        rejected.deferred = true;
        rejected.token = entry.token; rejected.array = entry.array;
        rejected.operation = entry.operation; rejected.elementOffset = entry.offset;
        rejected.elementCount = entry.count; rejected.status = CommandStatus::Error;
        queuedAnalogResponse(rejected);
        return;
    }
    auto* command = new ArrayCommand;
    command->deferred = true;
    command->operation = entry.operation;
    command->array = entry.array;
    command->token = token;
    command->elementOffset = entry.offset;
    command->elementCount = entry.count;
    command->payload = entry.data;
    traceAnalogQueue("issue", &entry);
    analog_->send(command);
}

void RiscvQemu::queuedAnalogResponse(const ArrayCommand& answer) {
    const auto found = analogEntries_.find(answer.token);
    if (found == analogEntries_.end()) throw std::runtime_error("unknown analog queue response token");
    auto& entry = found->second;
    auto& slot = bridge_.analogQueue().slots[entry.slot];
    if (answer.array != entry.array || answer.operation != entry.operation ||
        answer.elementOffset != entry.offset || answer.elementCount != entry.count ||
        slot.token != entry.queueToken)
        throw std::runtime_error("analog queue response identity mismatch");
    const bool admitting = analogAdmissionToken_ == entry.token;
    if (admitting && (!event_ || event_->stopReason != MITTENS_SYNC_STOP_ASQ_SUBMIT ||
                      event_->eventSequence != entry.token || event_->taskId != entry.slot))
        throw std::logic_error("analog response admission no longer owns CPU event");
    if (answer.status == CommandStatus::Busy) {
        if (!admitting || entry.accepted || !answer.payload.empty())
            throw std::runtime_error("invalid analog queue busy response");
        ++asqBusy_;
        traceAnalogQueue("busy", &entry);
        wake_->send(1, new SST::Event);
        return;
    }
    if (answer.status == CommandStatus::Accepted || answer.status == CommandStatus::Guaranteed) {
        if (!admitting || entry.accepted || !answer.payload.empty())
            throw std::runtime_error("invalid analog queue admission response");
        entry.accepted = true;
        entry.guaranteed = answer.status == CommandStatus::Guaranteed;
        traceAnalogQueue(entry.guaranteed ? "guaranteed" : "accepted", &entry);
        mittens_sync_store_release(&slot.state, MITTENS_SYNC_ASQ_INFLIGHT);
        if (entry.guaranteed) {
            ++asqGuaranteed_;
            analogAdmissionToken_ = 0;
            resume();
        } else ++asqFallback_;
        return;
    }
    if (answer.status == CommandStatus::Captured) {
        if (!entry.accepted || entry.captured || entry.operation == Operation::Store || !answer.payload.empty())
            throw std::runtime_error("invalid analog queue capture response");
        entry.captured = true;
        mittens_sync_store_release(&slot.source_captured, 1);
        traceAnalogQueue("captured", &entry);
        wakeAnalogQueue();
        return;
    }
    const bool success = answer.status == CommandStatus::Complete;
    if ((!success && answer.status != CommandStatus::Error) ||
        (!success && (entry.guaranteed || !admitting)) ||
        (success && (!entry.accepted || answer.payload.size() !=
            (entry.operation == Operation::Store ? entry.count * 4 : 0) ||
            (entry.operation != Operation::Store && !entry.captured))))
        throw std::runtime_error("invalid or imprecise analog queue completion");
    if (success) {
        std::copy(answer.payload.begin(), answer.payload.end(), slot.data);
        ++analogCommands_;
        ++asqCompleted_;
        if (entry.operation == Operation::Store) analogReadBytes_ += answer.payload.size();
        else analogWriteBytes_ += entry.count * 4;
    }
    slot.status = success ? 0 : 1;
    mittens_sync_store_release(&slot.state, success ? MITTENS_SYNC_ASQ_COMPLETE : MITTENS_SYNC_ASQ_ERROR);
    const auto completed = entry;
    analogActiveBytes_ -= entry.count * 4;
    analogEntries_.erase(found);
    traceAnalogQueue(success ? "complete" : "error", &completed);
    if (admitting) {
        analogAdmissionToken_ = 0;
        resume();
    } else wakeAnalogQueue();
}

void RiscvQemu::wakeAnalogQueue() {
    // Wake only an armed dispatch: never shorten the current CPU issue/cache
    // delay because some unrelated asynchronous command happened to finish.
    if (!asqWaiting_ || !analogQueueWaitSatisfied(asqWaitMask_, asqWaitAny_, asqWaitReason_)) return;
    const auto end = getCurrentSimTime(clock_);
    asqStallCycles_ += end - asqWaitStart_;
    if (RecordComponentObservations && analogWaitTrace_) {
        const char* reason = asqWaitReason_ == MITTENS_SYNC_ASQ_WAIT_REGISTER ? "register" :
            asqWaitReason_ == MITTENS_SYNC_ASQ_WAIT_FULL ? "full" : "drain";
        analogWaitTrace_ << asqWaitStart_ << ',' << end << ',' << reason << ',' << asqWaitStop_
            << ',' << asqWaitPc_ << ',' << asqWaitMask_ << ',' << asqWaitAny_ << '\n';
    }
    asqWaiting_ = false;
    wake_->send(0, new SST::Event);
}
}
