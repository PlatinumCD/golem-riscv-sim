#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace SST::Mittens
{

// Values only. The controller still owns and updates these counters/status;
// reporting does not need SST interfaces or access to live controller state.
struct TxCounters
{
    std::uint64_t networkTransmitPackets = 0;
    std::uint64_t networkTransmitWords = 0;
    std::uint64_t transmitDMAActiveCycles = 0;
    std::array<std::uint64_t, 5> transmitReadyCycles{};
    std::array<std::uint64_t, 5> transmitActiveLaneCycles{};
    std::array<std::uint64_t, 5> transmitReadyDirectionCycles{};
    std::uint64_t transmitIndependentReadyCycles = 0;
    std::uint64_t transmitMultipleActiveIndependentCycles = 0;
    std::uint64_t transmitSameDirectionReadyCycles = 0;
    std::uint64_t transmitSerializationStallCycles = 0;
    std::uint64_t transmitIndependentSerializationStallCycles = 0;
    std::uint64_t transmitSerializationDelayedBytes = 0;
    std::uint64_t transmitSerializationByteCycles = 0;
    std::uint64_t transmitFIFOEmptyCycles = 0;
    std::uint64_t transmitFIFOFullCycles = 0;
    std::uint64_t transmitFIFOEmptyLaneCycles = 0;
    std::uint64_t transmitFIFOFullLaneCycles = 0;
    std::uint64_t transmitQueueOccupancyCycleSum = 0;
    std::uint64_t transmitOpportunityObservedCycles = 0;
    std::uint32_t transmitMaximumQueueOccupancyObserved = 0;
    std::uint64_t networkWordHops = 0;
    std::uint64_t networkEndpointQueueTicks = 0;
    std::uint64_t transmitBlockedTicks = 0;
    std::uint64_t transmitBlockedEvents = 0;
    std::uint64_t transmitBlockedRetries = 0;
    std::uint64_t transmitMaximumQueueOccupancy = 0;
};

struct TxStatus
{
    // Preserve the existing progress counter: scalar plus T1 burst only.
    // T2/T4 lane occupancy is reported by the opportunity counters.
    std::size_t pendingNetwork = 0;
};

} // namespace SST::Mittens
