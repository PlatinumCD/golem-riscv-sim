#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "../../bridge/sharedMemoryBridge.h"

namespace SST::Mittens
{

// Values only. The controller still owns and updates these counters/status;
// reporting does not need SST interfaces or access to live controller state.
struct RxCounters
{
    std::uint64_t networkReceivePackets = 0;
    std::uint64_t networkReceiveWords = 0;
    std::uint64_t receiveDMATransfers = 0;
    std::uint64_t receiveDMAWords = 0;
    std::uint64_t receiveDMAActiveCycles = 0;
    std::uint64_t networkTransitTicks = 0;
};

// Values only: profiling must not expose queues, descriptors or transfers.
struct RxStatus
{
    std::size_t pendingNetwork = 0;
    std::size_t pendingTransfers = 0;
    std::uint64_t pendingDescriptors = 0;
    std::size_t completedFrames = 0;
    std::size_t readyBursts = 0;
    std::size_t incomingFrames = 0;
    std::uint32_t bridgeReceiveBursts = 0;
    bool authorizationAvailable = false;
    std::optional<ReceiveBurstInfo> bridgeHead;
    bool bridgeHeadScheduled = false;
    std::uint32_t firstUnscheduledPayloadOffset = UINT32_MAX;
    std::uint32_t firstUnscheduledPayloadSource = UINT32_MAX;
    std::uint32_t firstUnscheduledPayloadDescriptorCount = 0;
    std::uint32_t firstUnscheduledPayloadDescriptorRoute = UINT32_MAX;
    std::uint64_t firstUnscheduledPayloadDescriptorIteration = UINT64_MAX;
    std::uint32_t firstUnscheduledPayloadDescriptorRemainingWords = 0;
};

} // namespace SST::Mittens
