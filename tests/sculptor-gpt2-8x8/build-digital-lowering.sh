#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly SOURCE_DIR="${MITTENS_GPT2_ANALOG_LOWERING_DIR:-${BUILD_ROOT}/tests/sculptor-gpt2-8x8/fixture-lowering/full}"
readonly OUTPUT_DIR="${MITTENS_GPT2_DIGITAL_LOWERING_DIR:-${BUILD_ROOT}/tests/sculptor-gpt2-8x8/fixture-lowering/digital}"
readonly SCHEDULED="${SOURCE_DIR}/10-scheduled.mlir"
readonly PARTITIONED="${OUTPUT_DIR}/partitioned.mlir"
readonly OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"

require_file "${SCHEDULED}"
require_executable "${OPT}"
mkdir -p -- "${OUTPUT_DIR}"

"${OPT}" "${SCHEDULED}" \
    --sculptor-lower-scheduled-mvm-to-digital \
    --sculptor-fuse-task-graph \
    --sculptor-lower-golem-to-llvm-shims \
    --sculptor-partition-task-graph-by-core \
    -o "${PARTITIONED}"

if [[ "$(grep -c 'linalg.matmul_transpose_b' "${PARTITIONED}")" -ne 960 ]] ||
   grep -Eq 'sculptor\.array\.|golem_analog_mvm_(set|load|compute|store)' \
    "${PARTITIONED}"; then
    echo "GPT-2 digital lowering did not produce 960 pure digital matmuls" >&2
    exit 1
fi

SCULPTOR_PARTITIONED_MLIR="${PARTITIONED}" \
SCULPTOR_CORE_OBJECT_DIR="${OUTPUT_DIR}/cores" \
SCULPTOR_ACTIVE_CORE_MANIFEST="${OUTPUT_DIR}/active-cores.txt" \
SCULPTOR_CORE_LTO=none \
    "${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"

echo "GPT-2 scheduled MVM-to-digital lowering: ${OUTPUT_DIR}"
