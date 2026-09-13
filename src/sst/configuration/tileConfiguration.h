#pragma once
#include <cstdint>
#include <string>
#include "tileParameters.h"
namespace SST
{
class Params;
class Output;
} // namespace SST
namespace SST::Mittens
{
// Immutable after construction. Controllers consume resolved settings.
struct TileConfiguration
{
#define MITTENS_FIELD(type, field, name, value, category, help, documented) type field = value;
    MITTENS_TILE_PARAMETERS(MITTENS_FIELD)
#undef MITTENS_FIELD

    // Fixed execution contract, not selectable machine resources. Replay
    // parser unit tests construct snapshots directly; managed tiles validate
    // this contract before launching a guest.
    std::string memory = "16M";
    std::string memoryBackend = "streaming";
    bool memoryInitializationBatching = false;
    bool memoryAccessBatching = false;
    bool scratchpadAccessBatching = false;
    bool scratchpadAccessRunCompaction = false;
    bool memoryEventBatching = false;
    bool globalDMASubmitBatching = false;
    bool globalDMAMacroExecution = false;
    bool analogCommandBatching = false;
    std::uint32_t memoryAccessBatchRecords = 16;
    std::uint32_t memoryInitializationBytesPerCycle = 32;
    std::uint64_t memoryInitializationLatencyCycles = 2;
    std::uint64_t memoryInitializationInstructionQuantum = UINT64_C(67108864);
    bool networkTailDelivery = true;
    std::uint32_t memoryInitializationBarrierTiles = 0;
    bool scratchpadBoot = true;
    bool scratchpadEnabled = true;
    std::uint32_t qemuReadySetWorkers = 1;
    bool qemuRuntimeReadySet = false;
    bool qemuLocalLookahead = false;
    std::string qemuReadySetIndependenceProof = "";

    static TileConfiguration read(SST::Params& params);
    void validate(SST::Output& output) const;
    std::string writeResolved(const std::string& directory) const;
};
} // namespace SST::Mittens
