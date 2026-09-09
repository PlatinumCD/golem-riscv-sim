#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace SST::Mittens::Timing
{

struct Cpu
{
};
struct ReceiveDMA
{
};
struct Analog
{
};
struct Network
{
};

// Different domains cannot be interchanged without an explicit conversion.
struct Ticks
{
    std::uint64_t value;
};
template <class Domain> struct Cycles
{
    std::uint64_t value;
};

constexpr std::uint64_t add(std::uint64_t a, std::uint64_t b)
{
    if (b > UINT64_MAX - a)
        throw std::overflow_error("modeled time addition overflow");
    return a + b;
}
constexpr std::uint64_t multiply(std::uint64_t a, std::uint64_t b)
{
    if (a != 0 && b > UINT64_MAX / a)
        throw std::overflow_error("modeled time multiplication overflow");
    return a * b;
}
constexpr std::uint64_t ceilDivide(std::uint64_t value, std::uint64_t divisor)
{
    if (divisor == 0)
        throw std::invalid_argument("zero modeled clock divisor");
    return value / divisor + (value % divisor != 0);
}

template <class Domain> class Clock
{
  public:
    explicit constexpr Clock(std::uint64_t ticksPerCycle) : factor_(ticksPerCycle)
    {
        if (factor_ == 0)
            throw std::invalid_argument("zero modeled clock factor");
    }
    constexpr Ticks ticks(Cycles<Domain> cycles) const
    {
        return {multiply(cycles.value, factor_)};
    }
    // Floor reads the number of complete cycles that have elapsed. Ceil is
    // mandatory when scheduling a completion onto a different domain's clock.
    constexpr Cycles<Domain> floor(Ticks time) const
    {
        return {time.value / factor_};
    }
    constexpr Cycles<Domain> ceil(Ticks time) const
    {
        return {ceilDivide(time.value, factor_)};
    }
    constexpr Ticks after(Ticks origin, Cycles<Domain> duration) const
    {
        return {add(origin.value, ticks(duration).value)};
    }
    constexpr std::uint64_t factor() const noexcept
    {
        return factor_;
    }

  private:
    std::uint64_t factor_;
};
} // namespace SST::Mittens::Timing
