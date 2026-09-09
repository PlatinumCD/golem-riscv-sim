#pragma once

#include "tileMeasurements.h"

namespace SST::Mittens
{

// Pure reporting: consumes snapshots; no clocks, live queues, scheduling or IO.
ProgressSnapshot makeTileProgressSnapshot(const TileMeasurementSnapshot& data,
                                         const RxStatus& rx,
                                         const TxStatus& tx,
                                         std::uint64_t pendingMemoryRequests,
                                         std::uint64_t wallMilliseconds, const char* kind);
std::string formatTileProgress(std::uint32_t tileId, const ProgressSnapshot& progress);
std::string formatProgressWatchdog(std::uint32_t tileId, std::uint64_t timeoutMilliseconds,
                                  std::uint64_t elapsedMilliseconds, std::uint64_t deploymentEpoch,
                                  std::uint64_t initializationEpoch);

} // namespace SST::Mittens
