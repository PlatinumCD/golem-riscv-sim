#pragma once

#include <stdint.h>

#include "golem/runtime/tile_abi.h"

namespace mittens::materialized_test {

struct DMAAccounting {
  uint64_t full_requests = 0;
  uint64_t tail_requests = 0;
  uint64_t requests = 0;
  uint64_t bytes = 0;
};

bool validMaterializedABIShape(const golem::runtime::TileABI &abi);
bool expectedDMAAccounting(const golem::runtime::TileABI &abi,
                           DMAAccounting *accounting);
bool expectedShardIterations(const golem::runtime::TileABI &abi,
                             uint64_t *iterations);

} // namespace mittens::materialized_test
