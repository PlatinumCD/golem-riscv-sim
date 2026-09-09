#pragma once

#include "summarySnapshot.h"
#include "../configuration/tileConfiguration.h"
#include "../analog/analogController.h"
#include "../execution/cpuExecutionController.h"
#include "../memory/globalDMAClient.h"
#include "../memory/memoryAccessController.h"
#include "../network/rx/rxSnapshot.h"
#include "../network/tx/txSnapshot.h"
#include "../synchronization/initializationBarrierClient.h"

namespace SST::Mittens
{

// Cross-resource reporting consumes owned snapshots, never live queues or
// scheduling callbacks. Tile gathers these at its existing observation site.
struct TileMeasurementSnapshot
{
    TileConfiguration configuration;
    CpuExecutionLedger::Snapshot accounting{};
    CpuExecutionController::Statistics cpu{};
    MemoryAccessController::Statistics memory{};
    GlobalDMAClient::Statistics globalDMA{};
    AnalogController::Statistics analog{};
    InitializationBarrierClient::Snapshot barriers{};
    TxCounters tx{};
    RxCounters rx{};
    ScratchpadTimingStatistics scratchpad{};
    std::uint64_t finishTick = 0;
    bool cpuAvailable = false;
    bool memoryAvailable = false;
    bool globalDMAAvailable = false;
    bool analogAvailable = false;
    bool networkAvailable = false;
    bool scratchpadAvailable = false;
    MeasurementTimebase timebase;
    MeasurementProvenance provenance = compiledMeasurementProvenance();
};

void reportTileProfile(SST::Output& output, const TileMeasurementSnapshot& data);
void reportTransmitOpportunity(SST::Output& output, const TileMeasurementSnapshot& data);
SummarySnapshot makeTileSummary(const TileMeasurementSnapshot& data);
void writeTileSummary(PerformanceProfile& profile, SST::Output& output,
                      const TileMeasurementSnapshot& data);

} // namespace SST::Mittens
