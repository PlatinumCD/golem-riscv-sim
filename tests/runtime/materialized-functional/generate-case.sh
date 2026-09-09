#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 2 ]]; then
    echo "usage: $0 <case> <compiler-artifact-directory>" >&2
    exit 2
fi

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly CASE_NAME="$1"
readonly ARTIFACT_DIR="$2"
readonly CASE_MATRIX="${TEST_DIR}/cases.json"
readonly INPUT="${TEST_DIR}/fixtures/${CASE_NAME}.mlir"
readonly LOWERING_DIR="${ARTIFACT_DIR}/lowering"
readonly CORE_DIR="${ARTIFACT_DIR}/cores"
readonly ACTIVE_CORES="${ARTIFACT_DIR}/active-cores.txt"
readonly DEPLOYMENT_MANIFEST="${ARTIFACT_DIR}/deployment-manifest.json"

IFS=$'\t' read -r CASE_ID MESH_WIDTH MESH_HEIGHT DIGITAL_WORKERS MINIMUM_WORK \
    < <(python3 "${TEST_DIR}/case_config.py" "${CASE_MATRIX}" "${CASE_NAME}")
readonly CASE_ID MESH_WIDTH MESH_HEIGHT DIGITAL_WORKERS MINIMUM_WORK
require_file "${INPUT}"
mkdir -p -- "${ARTIFACT_DIR}" "${LOWERING_DIR}" "${CORE_DIR}"

SCULPTOR_INPUT_MLIR="${INPUT}" \
SCULPTOR_DEPLOYMENT_DIR="${LOWERING_DIR}" \
SCULPTOR_MESH_ROWS="${MESH_HEIGHT}" \
SCULPTOR_MESH_COLS="${MESH_WIDTH}" \
SCULPTOR_ARRAYS_PER_CORE=1 \
SCULPTOR_ARRAY_ROWS=8 \
SCULPTOR_ARRAY_COLS=8 \
SCULPTOR_DIGITAL_WORKERS="${DIGITAL_WORKERS}" \
SCULPTOR_DIGITAL_MINIMUM_WORK_ITEMS_PER_UNIT="${MINIMUM_WORK}" \
SCULPTOR_BALANCE_DIGITAL_WORK=1 \
SCULPTOR_DATAFLOW=sharded \
SCULPTOR_FIXED_SHARD_BYTES=4096 \
SCULPTOR_STREAMING_SCRATCHPAD_BYTES=2097152 \
SCULPTOR_GLOBAL_RAM_BYTES=34359738368 \
SCULPTOR_MAX_IN_FLIGHT=2 \
SCULPTOR_FUSE_POINTWISE_EPILOGUES=0 \
SCULPTOR_PRE_SPLIT_CORE_DIR="${CORE_DIR}" \
SCULPTOR_PRE_SPLIT_ACTIVE_CORE_MANIFEST="${ACTIVE_CORES}" \
SCULPTOR_PRE_SPLIT_DEPLOYMENT_MANIFEST="${DEPLOYMENT_MANIFEST}" \
SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS=300 \
    "${PROJECT_ROOT}/build-scripts/lower-sculptor-ra-tree.sh"

python3 "${TEST_DIR}/validate_generated_case.py" \
    --case "${CASE_NAME}" --matrix "${CASE_MATRIX}" \
    --formed "${LOWERING_DIR}/04-parametric-work.mlir" \
    --active-cores "${ACTIVE_CORES}" \
    --deployment-manifest "${DEPLOYMENT_MANIFEST}" \
    --extracted-directory "${CORE_DIR}"

SCULPTOR_DEPLOYMENT_MLIR="${LOWERING_DIR}/09-tile-deployment.mlir" \
SCULPTOR_CORE_OBJECT_DIR="${CORE_DIR}" \
SCULPTOR_ACTIVE_CORE_MANIFEST="${ACTIVE_CORES}" \
SCULPTOR_CORE_PRE_SPLIT=1 \
SCULPTOR_CORE_LTO=none \
SCULPTOR_CORE_SCRATCHPAD_BYTES=2097152 \
SCULPTOR_CORE_STRICT_MEMORY_AUDIT=1 \
SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS=300 \
    "${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"

require_file "${DEPLOYMENT_MANIFEST}"
mapfile -t active_tiles <"${ACTIVE_CORES}"
if [[ "${#active_tiles[@]}" -eq 0 ]]; then
    echo "${CASE_NAME}: compiler emitted no active tiles" >&2
    exit 1
fi
for tile in "${active_tiles[@]}"; do
    require_file "${CORE_DIR}/core-${tile}.o"
done
echo "materialized ${CASE_NAME}: compiler artifacts ready (${#active_tiles[@]} tiles)"
