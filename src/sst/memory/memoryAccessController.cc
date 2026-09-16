#include "memoryAccessController.h"
#include "memoryAccessCoalescer.h"
namespace SST::Mittens
{
CpuDeviceResult MemoryAccessController::executeInstruction(const CpuInstructionAction& action)
{
    if (!instructionCache_)
        throw std::logic_error("instruction cache is disabled");
    if (action.invalidate)
    {
        if (action.bytes != 0 || instructionDeadline_)
            throw std::logic_error("invalid instruction-cache fence");
        instructionCache_->invalidate();
        ++instructionInvalidations_;
        return {true, {}, {}};
    }
    if (instructionDeadline_)
    {
        if (instructionDeadline_->step != action.step)
            throw std::logic_error("instruction-cache deadline belongs to another CPU step");
        if (host_.now().value < instructionDeadline_->tick.value)
            throw std::logic_error("instruction fetch resumed before its cache fill completed");
        instructionDeadline_.reset();
        return {true, {}, {}};
    }
    const auto start = std::max(action.cursor.value, cpuDomain().ceil(host_.now()).value);
    if (action.count == 0 || action.count > config_.instructionFetchSegmentSize ||
        (action.count > 1 && action.bytes != 4))
        throw std::invalid_argument("invalid fetch segment proposal");
    auto fetch = instructionCache_->fetch(action.address, action.bytes, start);
    std::uint32_t count = 1;
    if (fetch.hit && action.count > 1 && host_.instructionSegmentSafe && host_.instructionSegmentSafe()) {
        for (; count < action.count; ++count) {
            const auto address = action.address + 4 * count;
            if (!instructionCache_->resident(address, 4)) break;
            fetch = instructionCache_->fetch(address, 4, fetch.readyCycle);
        }
    }
    const auto cycles = fetch.readyCycle - start;
    // Each lookup includes one issue cycle. Only the remainder is a fetch
    // stall; CpuExecutionController reconciles all approved issue cycles.
    instructionStallCycles_ = Timing::add(instructionStallCycles_, cycles - count);
    instructionDeadline_ = ScratchpadAccessDeadline{
        action.step, cpuDomain().ticks({fetch.readyCycle})};
    return {false, Timing::Cycles<Timing::Cpu>{
                fetch.readyCycle - cpuDomain().floor(host_.now()).value},
            Timing::Cycles<Timing::Cpu>{fetch.readyCycle}, false, count};
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
    throw std::invalid_argument("CPU data access outside executable SPM");
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
