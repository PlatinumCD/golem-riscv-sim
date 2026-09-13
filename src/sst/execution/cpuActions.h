#pragma once
#include "clockDomain.h"
#include <mittens/SyncTileBridge.h>
#include <cstdint>
#include <optional>
#include <vector>
namespace SST::Mittens
{
// Values crossing the execution/device boundary never contain replay queues.
struct CpuMemoryAction
{
    std::uint64_t address, pc, returnAddress;
    std::uint32_t size, flags;
    std::vector<MittensSyncMemoryAccess> group;
    Timing::Cycles<Timing::Cpu> cursor;
    std::uint64_t step = 0;
};
struct CpuAnalogAction
{
    std::uint32_t reason, array, flags;
    std::uint64_t sequence;
};
struct CpuInstructionAction
{
    std::uint64_t address;
    std::uint32_t bytes;
    bool invalidate;
    Timing::Cycles<Timing::Cpu> cursor;
    std::uint64_t step;
};
struct CpuNetworkAction
{
    std::uint32_t reason, flags, source, route, words;
    std::uint64_t iteration, address;
};
struct CpuGlobalDMAAction
{
    std::uint32_t reason, flags, direction, token, bytes, requestFlags;
    std::uint64_t execution, globalOffset, scratchpadOffset, iteration;
    std::vector<MittensSyncGlobalDMASubmit> waits;
    Timing::Cycles<Timing::Cpu> cursor;
};
struct CpuBarrierAction
{
    std::uint32_t epoch, contribution, flags;
};
struct CpuInitializationAction
{
    std::uint64_t accesses, reads, writes;
    std::uint32_t size, memoryFlags;
};
struct CpuTaskAction
{
    std::uint32_t task;
    std::uint64_t execution, sequence;
    bool finish;
};
struct CpuDeviceResult
{
    bool complete = false;
    std::optional<Timing::Cycles<Timing::Cpu>> delay;
    std::optional<Timing::Cycles<Timing::Cpu>> cursor;
    // Buffered writes historically advance internally but report blocked to
    // the enclosing wake handler, retaining its watchdog observation site.
    bool reportBlockedAfterCompletion = false;
};

} // namespace SST::Mittens
