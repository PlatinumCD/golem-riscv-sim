#pragma once

#include "scratchpad/scratchpadTimingModel.h"

#include <cstdint>
#include <vector>

namespace SST::Mittens
{

struct InstructionCacheConfiguration
{
    std::uint64_t capacityBytes = 8192;
    std::uint32_t lineBytes = 64;
    std::uint32_t ways = 2;
    std::uint64_t hitLatencyCycles = 1;
};

struct InstructionFetchResult
{
    bool hit;
    std::uint64_t readyCycle;
    std::uint64_t fillBytes;
};

struct InstructionCacheStatistics
{
    std::uint64_t fetches = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t fillBytes = 0;
};

class InstructionCache final
{
  public:
    InstructionCache(std::uint64_t spmBase, InstructionCacheConfiguration configuration,
                     ScratchpadTimingModel& scratchpad);

    InstructionFetchResult fetch(std::uint64_t address, std::uint32_t bytes,
                                 std::uint64_t currentCycle);
    void invalidate() noexcept;
    const InstructionCacheStatistics& statistics() const noexcept
    {
        return statistics_;
    }
    const InstructionCacheConfiguration& configuration() const noexcept
    {
        return configuration_;
    }

  private:
    struct Line
    {
        std::uint64_t tag = 0;
        std::uint64_t age = 0;
        bool valid = false;
    };
    std::uint64_t spmBase_;
    InstructionCacheConfiguration configuration_;
    ScratchpadTimingModel& scratchpad_;
    std::uint64_t sets_;
    std::uint64_t age_ = 0;
    std::uint64_t busyUntil_ = 0;
    std::vector<Line> lines_;
    InstructionCacheStatistics statistics_;
};

} // namespace SST::Mittens
