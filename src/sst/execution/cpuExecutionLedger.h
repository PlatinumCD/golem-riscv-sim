#pragma once
#include "clockDomain.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace SST::Mittens
{
// Cumulative grant counters and issue-region occupancy have different lifetimes.
// This class has no transport, simulator, device, or logging dependencies.
class CpuExecutionLedger final
{
  public:
    struct Counts
    {
        std::uint64_t instructions = 0, vectors = 0;
    };
    struct Snapshot
    {
        std::uint64_t epoch = 0, budget = 0;
        Counts baseline, region, total;
        std::uint64_t regionCycles = 0, totalCycles = 0;
    };
    struct Charge
    {
        Counts retired;
        Timing::Cycles<Timing::Cpu> cycles;
    };
    explicit CpuExecutionLedger(std::uint64_t issueWidth) : width_(issueWidth)
    {
        if (width_ == 0)
            throw std::invalid_argument("zero CPU issue width");
    }
    Snapshot snapshot() const noexcept
    {
        return state_;
    }
    void beginGrant(std::uint64_t epoch, std::uint64_t budget)
    {
        if (budget == 0)
            throw std::invalid_argument("zero CPU grant budget");
        state_.epoch = epoch;
        state_.budget = budget;
        state_.baseline = {};
    }
    void validateCaptured(std::uint64_t epoch, Counts target) const
    {
        if (epoch != state_.epoch || target.instructions > state_.budget ||
            target.vectors > target.instructions ||
            target.instructions < state_.baseline.instructions ||
            target.vectors < state_.baseline.vectors)
            throw std::runtime_error("invalid CPU grant epoch or cumulative counters");
    }
    Charge previewTo(Counts target) const
    {
        auto copy = *this;
        const auto charge = copy.accountTo(target, false);
        return charge;
    }
    // Independently validates cumulative and delta counter consistency. Epoch
    // and budget belong to validateCaptured(); replay endpoints use this same
    // arithmetic after their transport envelope has passed that validation.
    Charge accountTo(Counts target, bool architecturalBoundary)
    {
        if (target.instructions < state_.baseline.instructions ||
            target.vectors < state_.baseline.vectors)
            throw std::runtime_error("CPU accounting target went backwards");
        const Counts delta{target.instructions - state_.baseline.instructions,
                           target.vectors - state_.baseline.vectors};
        if (target.vectors > target.instructions || delta.vectors > delta.instructions)
            throw std::runtime_error("CPU vector delta exceeds total instruction delta: epoch=" +
                std::to_string(state_.epoch) + " baseline=" +
                std::to_string(state_.baseline.instructions) + "/" +
                std::to_string(state_.baseline.vectors) + " target=" +
                std::to_string(target.instructions) + "/" + std::to_string(target.vectors));
        const Counts region{Timing::add(state_.region.instructions, delta.instructions),
                            Timing::add(state_.region.vectors, delta.vectors)};
        const auto cycles =
            std::max(Timing::ceilDivide(region.instructions, width_), region.vectors);
        if (cycles < state_.regionCycles)
            throw std::runtime_error("CPU run cycles went backwards");
        const auto elapsed = cycles - state_.regionCycles;
        const Counts total{Timing::add(state_.total.instructions, delta.instructions),
                           Timing::add(state_.total.vectors, delta.vectors)};
        const auto totalCycles = Timing::add(state_.totalCycles, elapsed);
        state_.baseline = target;
        state_.region = architecturalBoundary ? Counts{} : region;
        state_.regionCycles = architecturalBoundary ? 0 : cycles;
        // Reject invalid overflow atomically; valid execution timing is unchanged.
        state_.total = total;
        state_.totalCycles = totalCycles;
        return {delta, {elapsed}};
    }

  private:
    std::uint64_t width_;
    Snapshot state_;
};
} // namespace SST::Mittens
