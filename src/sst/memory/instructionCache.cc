#include "instructionCache.h"

#include "addressRegion.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace SST::Mittens
{
namespace
{
bool powerOfTwo(std::uint64_t value)
{
    return value != 0 && !(value & (value - 1));
}
std::uint64_t addChecked(std::uint64_t a, std::uint64_t b)
{
    if (a > std::numeric_limits<std::uint64_t>::max() - b)
        throw std::overflow_error("instruction cache cycle overflow");
    return a + b;
}
} // namespace

InstructionCache::InstructionCache(std::uint64_t spmBase,
                                   InstructionCacheConfiguration configuration,
                                   ScratchpadTimingModel& scratchpad)
    : spmBase_(spmBase), configuration_(configuration), scratchpad_(scratchpad)
{
    if (!powerOfTwo(configuration_.capacityBytes) || configuration_.capacityBytes == 0 ||
        !powerOfTwo(configuration_.lineBytes) || configuration_.lineBytes < 4 ||
        configuration_.ways == 0 || !powerOfTwo(configuration_.ways) ||
        configuration_.ways >
            std::numeric_limits<std::uint64_t>::max() / configuration_.lineBytes ||
        configuration_.hitLatencyCycles == 0 || spmBase % configuration_.lineBytes != 0)
        throw std::invalid_argument("invalid instruction cache configuration");
    const auto spmBytes = scratchpad_.configuration().capacityBytes;
    if (configuration_.lineBytes > spmBytes || spmBytes % configuration_.lineBytes != 0)
        throw std::invalid_argument("invalid instruction cache SPM geometry");
    const auto setSpan = static_cast<std::uint64_t>(configuration_.lineBytes) * configuration_.ways;
    if (configuration_.capacityBytes < setSpan || configuration_.capacityBytes % setSpan != 0)
        throw std::invalid_argument("invalid instruction cache configuration");
    sets_ = configuration_.capacityBytes / setSpan;
    lines_.resize(sets_ * configuration_.ways);
    if (!AddressRegion::representable(spmBase_, spmBytes))
        throw std::invalid_argument("instruction cache SPM address range overflows");
}

InstructionFetchResult InstructionCache::fetch(std::uint64_t address, std::uint32_t bytes,
                                               std::uint64_t currentCycle)
{
    if ((bytes != 2 && bytes != 4) ||
        !AddressRegion{spmBase_, scratchpad_.configuration().capacityBytes}.containsRange(address,
                                                                                          bytes))
        throw std::out_of_range("instruction fetch is invalid or outside SPM");
    if ((address & 1) != 0)
        throw std::invalid_argument("instruction PC is not halfword aligned");
    if (currentCycle < busyUntil_)
        throw std::logic_error("instruction cache fetch overlaps a blocking miss or hit");
    ++statistics_.fetches;
    const auto relative = address - spmBase_;
    const auto firstLine = relative / configuration_.lineBytes;
    const auto lastLine = (relative + bytes - 1) / configuration_.lineBytes;
    std::uint64_t ready = addChecked(currentCycle, configuration_.hitLatencyCycles);
    std::uint64_t filled = 0;
    bool hit = true;
    for (auto lineNumber = firstLine; lineNumber <= lastLine; ++lineNumber)
    {
        const auto set = lineNumber % sets_;
        const auto tag = lineNumber / sets_;
        auto begin = lines_.begin() + set * configuration_.ways;
        auto end = begin + configuration_.ways;
        auto found = std::find_if(begin, end, [tag](const Line& line)
                                  { return line.valid && line.tag == tag; });
        if (found == end)
        {
            hit = false;
            auto victim = std::find_if(begin, end, [](const Line& line) { return !line.valid; });
            if (victim == end)
            {
                victim = std::min_element(begin, end, [](const Line& a, const Line& b)
                                          { return a.age < b.age; });
            }
            const auto schedule = scratchpad_.scheduleInstructionFetch(
                ready, lineNumber * configuration_.lineBytes, configuration_.lineBytes);
            ready = std::max(ready, schedule.completionCycle);
            filled += configuration_.lineBytes;
            victim->tag = tag;
            victim->valid = true;
            victim->age = ++age_;
        }
        else
        {
            found->age = ++age_;
        }
    }
    if (hit)
        ++statistics_.hits;
    else
        ++statistics_.misses;
    statistics_.fillBytes += filled;
    busyUntil_ = ready;
    return {hit, ready, filled};
}

void InstructionCache::invalidate() noexcept
{
    for (auto& line : lines_)
        line.valid = false;
}
} // namespace SST::Mittens
