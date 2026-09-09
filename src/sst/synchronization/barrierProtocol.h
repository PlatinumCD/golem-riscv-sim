#pragma once
#include <cstdint>
namespace SST::Mittens
{
enum class EpochBarrierMessage : std::uint32_t
{
    Arrive = 0,
    Release = 1,
    PrefixStop = 2,
};

enum class EpochBarrierContribution : std::uint32_t
{
    None = 0,
    WorkComplete = 1,
    Idle = 2,
};

enum class MemoryInitializationBarrierMessage : std::uint32_t
{
    Arrive = 0,
    Release = 1,
};

} // namespace SST::Mittens
