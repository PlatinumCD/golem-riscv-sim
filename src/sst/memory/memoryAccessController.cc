#include "memoryAccessController.h"
#include "memoryAccessCoalescer.h"
namespace SST::Mittens
{
bool MemoryAccessController::memoryReadHasPendingWriteHazard(std::uint64_t address,
                                                             std::uint32_t size) const noexcept
{
    const std::uint64_t readBegin = address;
    const std::uint64_t readEnd = readBegin > UINT64_MAX - size ? UINT64_MAX : readBegin + size;
    for (const auto& entry : pendingMemoryRequests_)
    {
        const PendingMemoryRequest& pending = entry.second;
        if (!pending.write)
        {
            continue;
        }
        const std::uint64_t writeEnd = pending.address > UINT64_MAX - pending.size
                                           ? UINT64_MAX
                                           : pending.address + pending.size;
        if (readBegin < writeEnd && pending.address < readEnd)
        {
            return true;
        }
    }
    return false;
}

std::uint64_t MemoryAccessController::sendMemoryRequest(const CpuMemoryAction& action)
{
    const std::uint32_t maximumAccessSize =
        host_.hasMemoryReplay() ? config_.memoryCacheLineSize : 16;
    if (action.size == 0 || action.size > maximumAccessSize ||
        (action.flags & ~MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0)
    {
        output_.fatal(

            -1,
            "tile %u received invalid memory request "
            "(address=0x%llx, size=%u, flags=0x%x)\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned long long>(action.address),
            static_cast<unsigned>(action.size), static_cast<unsigned>(action.flags));
    }

    const bool write = (action.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0;
    std::uint64_t timingAddress = action.address;
    if (config_.memoryTileStride != 0)
    {
        const AddressRegion memory{config_.memoryGuestBase, config_.memoryTileStride};
        if (!memory.containsRange(action.address, action.size) ||
            config_.tileId > (UINT64_MAX - (action.address - config_.memoryGuestBase)) /
                                 config_.memoryTileStride)
        {
            output_.fatal(

                -1,
                "tile %u cannot translate memory address 0x%llx "
                "with base 0x%llx and stride 0x%llx\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(action.address),
                static_cast<unsigned long long>(config_.memoryGuestBase),
                static_cast<unsigned long long>(config_.memoryTileStride));
        }
        timingAddress = static_cast<std::uint64_t>(config_.tileId) * config_.memoryTileStride +
                        (action.address - config_.memoryGuestBase);
    }
    auto request = host_.prepare(timingAddress, action.size, write);
    if (!request)
        throw std::logic_error("memory transport returned no prepared request");
    if (write)
        ++statistics_.memoryWrites_;
    else
        ++statistics_.memoryReads_;
    const auto context = host_.context();

    const PendingMemoryRequest pending{
        action.step,    host_.now().value,    action.address, timingAddress,
        action.pc,      action.returnAddress, action.size,    write,
        context.taskId, context.executionId,  context.phase,
    };
    const auto inserted = pendingMemoryRequests_.emplace(request->id(), pending);
    if (!inserted.second)
    {
        output_.fatal(

            -1, "tile %u reused pending memory request ID %llu\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned long long>(request->id()));
    }
    statistics_.maximumOutstandingMemoryRequests_ =
        std::max(statistics_.maximumOutstandingMemoryRequests_,
                 static_cast<std::uint64_t>(pendingMemoryRequests_.size()));
    if (!write)
    {
        ++statistics_.outstandingMemoryReads_;
        statistics_.maximumOutstandingMemoryReads_ =
            std::max(statistics_.maximumOutstandingMemoryReads_,
                     static_cast<std::uint64_t>(statistics_.outstandingMemoryReads_));
    }
    ++statistics_.memoryRequests_;
    performanceProfile_.recordMemory("issue", request->id(), action.address, timingAddress,
                                     pending.programCounter, pending.returnAddress, action.size,
                                     write, pending.taskId, pending.executionId,
                                     pending.phase.c_str(), pending.issueTick, pending.issueTick);
    request->send();

    return request->id();
}

bool MemoryAccessController::issueMemoryRequest(const CpuMemoryAction& action)
{
    const bool write = (action.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0;
    const std::uint64_t requestId = sendMemoryRequest(action);

    if (!write)
    {
        blockingMemoryRequestId_ = requestId;
        blockingMemoryStep_ = action.step;
        return false;
    }

    ++statistics_.outstandingMemoryWrites_;
    statistics_.maximumStoreBufferOccupancy_ =
        std::max(statistics_.maximumStoreBufferOccupancy_,
                 static_cast<std::uint64_t>(statistics_.outstandingMemoryWrites_));
    if (statistics_.outstandingMemoryWrites_ >= config_.memoryStoreBufferEntries)
    {
        ++statistics_.memoryStoreBufferFullEvents_;
        blockingMemoryRequestId_ = requestId;
        blockingMemoryStep_ = action.step;
        return false;
    }

    /*
     * QEMU performs the architectural store immediately after this resume.
     * The StandardMem write continues independently as a timing-only store
     * buffer entry.  Reads remain blocking because the bridge does not yet
     * carry register-dependency information.
     */
    return true;
}

void MemoryAccessController::onResponse(std::uint64_t requestId)
{
    const auto pending = pendingMemoryRequests_.find(requestId);
    if (pending == pendingMemoryRequests_.end())
    {
        output_.fatal(

            -1, "tile %u received unexpected memory response %s\n",
            static_cast<unsigned>(config_.tileId), std::to_string(requestId).c_str());
    }
    const PendingMemoryRequest metadata = pending->second;

    performanceProfile_.recordMemory(
        "response", requestId, metadata.address, metadata.timingAddress, metadata.programCounter,
        metadata.returnAddress, metadata.size, metadata.write, metadata.taskId,
        metadata.executionId, metadata.phase.c_str(), metadata.issueTick, host_.now().value);
    const bool blockingRequestCompleted =
        blockingMemoryRequestId_.has_value() && *blockingMemoryRequestId_ == requestId;
    const bool groupedRequestCompleted = cpuMemoryGroupRequestIds_.erase(requestId) != 0;
    if (metadata.write)
    {
        if (statistics_.outstandingMemoryWrites_ == 0)
        {
            output_.fatal(

                -1, "tile %u completed a store with no outstanding stores\n",
                static_cast<unsigned>(config_.tileId));
        }
        --statistics_.outstandingMemoryWrites_;
    }
    else
    {
        if (statistics_.outstandingMemoryReads_ == 0)
        {
            output_.fatal(-1, "tile %u completed a load with no outstanding loads\n",
                          static_cast<unsigned>(config_.tileId));
        }
        --statistics_.outstandingMemoryReads_;
    }
    pendingMemoryRequests_.erase(pending);
    ++statistics_.memoryResponses_;

    if (groupedRequestCompleted)
    {
        if (cpuMemoryGroupRequestIds_.empty())
        {
            host_.completeMemory(metadata.cpuStep, true);
        }
        return;
    }

    if (blockingRequestCompleted)
    {
        blockingMemoryRequestId_.reset();
        host_.completeMemory(blockingMemoryStep_, false);
        return;
    }

    if (blockingMemoryRequestId_.has_value() && host_.pending().has_value() &&
        host_.pending()->stopReason == MITTENS_SYNC_STOP_MEMORY_ACCESS &&
        (host_.pending()->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0 &&
        statistics_.outstandingMemoryWrites_ < config_.memoryStoreBufferEntries)
    {
        blockingMemoryRequestId_.reset();
        host_.completeMemory(blockingMemoryStep_, false);
        return;
    }

    if (host_.pending().has_value() && statistics_.outstandingMemoryWrites_ == 0)
    {
        (void)host_.retry();
        return;
    }
    if (host_.pending().has_value() &&
        host_.pending()->stopReason == MITTENS_SYNC_STOP_MEMORY_ACCESS &&
        (host_.pending()->memoryFlags & MITTENS_SYNC_MEMORY_FLAG_WRITE) == 0 &&
        !memoryReadHasPendingWriteHazard(host_.pending()->memoryAddress,
                                         host_.pending()->memorySize))
    {
        (void)host_.retry();
    }
}

CpuDeviceResult MemoryAccessController::executeCpuMemory(const CpuMemoryAction& action)
{
    CpuDeviceResult result;
    auto scratchpadTimingCycle_ = action.cursor.value;
    const auto finish = [&](bool complete)
    {
        result.complete = complete;
        result.cursor = Timing::Cycles<Timing::Cpu>{scratchpadTimingCycle_};
        return result;
    };
    const auto scheduleDelay = [&](std::uint64_t cycles)
    { result.delay = Timing::Cycles<Timing::Cpu>{cycles}; };
    if ((action.flags & MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD) != 0)
    {
        if (scratchpadTimingModel_ == nullptr ||
            !scratchpadRegion().containsRange(action.address, action.size))
        {
            output_.fatal(-1,
                          "tile %u received a scratchpad access while the "
                          "scratchpad is disabled\n",
                          static_cast<unsigned>(config_.tileId));
        }
        if (!scratchpadAccessDeadline_)
        {
            const ScratchpadSchedule schedule = scratchpadTimingModel_->scheduleCPU(
                scratchpadTimingCycle_, scratchpadRegion().offset(action.address), action.size,
                (action.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0);
            scratchpadAccessDeadline_ = ScratchpadAccessDeadline{
                action.step,
                cpuDomain().after(Timing::Ticks{host_.now().value}, {schedule.serviceCycles})};
            scratchpadTimingCycle_ = schedule.completionCycle;
            scheduleDelay(schedule.serviceCycles);
            return finish(false);
        }
        if (scratchpadAccessDeadline_->step != action.step)
        {
            throw std::logic_error("SPM access deadline belongs to a different CPU step");
        }
        if (host_.now().value < scratchpadAccessDeadline_->tick.value)
        {
            scheduleDelay(
                cpuDomain()
                    .ceil(Timing::Ticks{scratchpadAccessDeadline_->tick.value - host_.now().value})
                    .value);
            return finish(false);
        }
        scratchpadAccessDeadline_.reset();
        return finish(true);
    }
    if (!host_.prepare)
    {
        output_.fatal(

            -1,
            "tile %u received a memory event without a "
            "memHierarchy backend\n",
            static_cast<unsigned>(config_.tileId));
    }
    if ((action.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) == 0 &&
        memoryReadHasPendingWriteHazard(action.address, action.size))
    {
        return finish(false);
    }
    if (action.group.size() > 1)
    {
        if (!cpuMemoryGroupRequestIds_.empty())
        {
            return finish(false);
        }
        for (std::size_t index = 0; index < action.group.size(); ++index)
        {
            const MittensSyncMemoryAccess& access = action.group[index];
            CpuMemoryAction groupAction{access.address,
                                        access.program_counter,
                                        access.return_address,
                                        access.size,
                                        access.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE,
                                        {},
                                        action.cursor,
                                        action.step};
            if (memoryReadHasPendingWriteHazard(groupAction.address, groupAction.size))
            {
                return finish(false);
            }
        }
        for (std::size_t index = 0; index < action.group.size(); ++index)
        {
            const MittensSyncMemoryAccess& access = action.group[index];
            CpuMemoryAction groupAction{access.address,
                                        access.program_counter,
                                        access.return_address,
                                        access.size,
                                        access.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE,
                                        {},
                                        action.cursor,
                                        action.step};
            cpuMemoryGroupRequestIds_.insert(sendMemoryRequest(groupAction));
        }
        const std::uint64_t groupSize = action.group.size();
        if (sameDynamicMemoryInstruction(action.group[0], action.group[1]))
        {
            ++statistics_.vectorMemoryRequestGroups_;
            statistics_.vectorMemoryGroupRequests_ += groupSize;
        }
        else
        {
            ++statistics_.scalarMemoryRequestGroups_;
            statistics_.scalarMemoryGroupRequests_ += groupSize;
        }
        return finish(false);
    }
    if (!blockingMemoryRequestId_.has_value())
    {
        const bool complete = issueMemoryRequest(action);
        result.reportBlockedAfterCompletion = complete;
        return finish(complete);
    }
    return finish(false);
}

ScratchpadSchedule MemoryAccessController::reserveCPU(const MittensSyncMemoryAccess& access,
                                                      std::uint64_t cursor)
{
    if (!scratchpadTimingModel_)
        throw std::logic_error("SPM is disabled");
    const bool write = (access.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0;
    if (access.flags & MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION)
        return scratchpadTimingModel_->scheduleCPU(
            cursor, scratchpadRegion().offset(access.address),
            sizeof(std::uint32_t) * access.repeat_count, write);
    if (access.repeat_count == 1)
        return scratchpadTimingModel_->scheduleCPU(
            cursor, scratchpadRegion().offset(access.address), access.size, write);
    return scratchpadTimingModel_->scheduleCPUContiguousRun(
        cursor, scratchpadRegion().offset(access.address), access.size, access.repeat_count, write);
}

} // namespace SST::Mittens
