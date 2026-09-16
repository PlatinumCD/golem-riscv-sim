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
    // Executable SPM, its instruction cache, and streaming DMA are mandatory.
    // Parameters below describe resource sizes/timing, not alternate backends.
#define MITTENS_FIELD(type, field, name, value, category, help, documented) type field = value;
    MITTENS_TILE_PARAMETERS(MITTENS_FIELD)
#undef MITTENS_FIELD

    // Fixed execution contract, not selectable machine resources. Replay
    // parser unit tests construct snapshots directly; managed tiles validate
    // this contract before launching a guest.
    std::string memory = "16M";
    bool memoryAccessBatching = false;
    bool scratchpadAccessBatching = false;
    bool scratchpadAccessRunCompaction = false;
    bool memoryEventBatching = false;
    bool globalDMASubmitBatching = false;
    bool globalDMAMacroExecution = false;
    bool analogCommandBatching = false;
    std::uint32_t memoryAccessBatchRecords = 16;
    bool networkTailDelivery = true;
    std::uint32_t qemuLocalLookaheadWorkers = 1;
    bool qemuLocalLookahead = false;
    std::string qemuLocalLookaheadIndependenceProof = "";

    static TileConfiguration read(SST::Params& params);
    void validate(SST::Output& output) const;
    std::string writeResolved(const std::string& directory) const;
};
} // namespace SST::Mittens
