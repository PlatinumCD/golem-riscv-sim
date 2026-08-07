#!/usr/bin/env bash
set -euo pipefail

# Lower one tensor-level MLIR module through Sculptor's RA-tree deployment
# boundary.  The output contains placed tile modules and is the input to
# build-sculptor-core-objects.sh.

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly INPUT="${SCULPTOR_INPUT_MLIR:?SCULPTOR_INPUT_MLIR must name tensor-level input MLIR}"
readonly OUTPUT_DIR="${SCULPTOR_DEPLOYMENT_DIR:?SCULPTOR_DEPLOYMENT_DIR must name an output directory}"
readonly OPT="${SCULPTOR_OPT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt}"
readonly MESH_ROWS="${SCULPTOR_MESH_ROWS:-1}"
readonly MESH_COLS="${SCULPTOR_MESH_COLS:-1}"
readonly ARRAYS_PER_CORE="${SCULPTOR_ARRAYS_PER_CORE:-1}"
readonly ARRAY_ROWS="${SCULPTOR_ARRAY_ROWS:-8}"
readonly ARRAY_COLS="${SCULPTOR_ARRAY_COLS:-8}"
readonly DIGITAL_WORKERS="${SCULPTOR_DIGITAL_WORKERS:-1}"
readonly DUPLICATE_MATRICES="${SCULPTOR_DUPLICATE_MATRICES:-0}"
readonly BALANCE_DIGITAL_WORK="${SCULPTOR_BALANCE_DIGITAL_WORK:-0}"
readonly PLANNER_STRATEGIES="${SCULPTOR_PLANNER_STRATEGIES:-setup-first,mvm-wave,fan-out-cut,consumer-bound-fill}"
readonly PLACEMENT_SCHEDULE="${SCULPTOR_PLACEMENT_SCHEDULE:-greedy}"

require_file "${INPUT}"
require_executable "${OPT}"

for value in "${MESH_ROWS}" "${MESH_COLS}" "${ARRAYS_PER_CORE}" \
    "${ARRAY_ROWS}" "${ARRAY_COLS}" "${DIGITAL_WORKERS}"; do
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "RA-tree hardware values must be positive integers" >&2
        exit 2
    fi
done
if [[ "${DUPLICATE_MATRICES}" != 0 && "${DUPLICATE_MATRICES}" != 1 ]]; then
    echo "SCULPTOR_DUPLICATE_MATRICES must be 0 or 1" >&2
    exit 2
fi
if [[ "${BALANCE_DIGITAL_WORK}" != 0 && "${BALANCE_DIGITAL_WORK}" != 1 ]]; then
    echo "SCULPTOR_BALANCE_DIGITAL_WORK must be 0 or 1" >&2
    exit 2
fi

mkdir -p -- "${OUTPUT_DIR}"

run_stage() {
    local input="$1"
    local output="$2"
    shift 2
    "${OPT}" "${input}" --verify-each "$@" -o "${output}"
}

run_stage "${INPUT}" "${OUTPUT_DIR}/01-canonical.mlir" \
    --sculptor-canonicalize-layers \
    --sculptor-extract-layers
run_stage "${OUTPUT_DIR}/01-canonical.mlir" "${OUTPUT_DIR}/02-converted.mlir" \
    --sculptor-convert-layers
run_stage "${OUTPUT_DIR}/02-converted.mlir" "${OUTPUT_DIR}/03-golem.mlir" \
    "--sculptor-expand-mvm-to-golem=array-rows=${ARRAY_ROWS} array-cols=${ARRAY_COLS}"
golem_input="${OUTPUT_DIR}/03-golem.mlir"
if [[ "${DUPLICATE_MATRICES}" == 1 ]]; then
    run_stage "${golem_input}" "${OUTPUT_DIR}/03-duplicate-matrices.mlir" \
        --sculptor-duplicate-matrices
    golem_input="${OUTPUT_DIR}/03-duplicate-matrices.mlir"
fi
run_stage "${golem_input}" "${OUTPUT_DIR}/04-digital-work.mlir" \
    "--sculptor-expand-digital-work=parallel-workers=${DIGITAL_WORKERS}"
run_stage "${OUTPUT_DIR}/04-digital-work.mlir" "${OUTPUT_DIR}/05-ra-tree.mlir" \
    --sculptor-build-ra-tree
mapping_options="strategies=${PLANNER_STRATEGIES} mesh-rows=${MESH_ROWS} mesh-cols=${MESH_COLS} arrays-per-core=${ARRAYS_PER_CORE} array-rows=${ARRAY_ROWS} array-cols=${ARRAY_COLS} verify-plan"
if [[ "${BALANCE_DIGITAL_WORK}" == 1 ]]; then
    mapping_options+=" balance-digital-work"
fi
run_stage "${OUTPUT_DIR}/05-ra-tree.mlir" "${OUTPUT_DIR}/06-mapping-plan.mlir" \
    "--sculptor-plan-mapping=${mapping_options}"
# The current tile outliner consumes the RA tree and logical-tile graph that
# plan-mapping emits.  apply-mapping-plan intentionally consumes those
# attributes, so it is not a deployment stage yet.
run_stage "${OUTPUT_DIR}/06-mapping-plan.mlir" "${OUTPUT_DIR}/08-placed.mlir" \
    "--sculptor-place-logical-tiles=schedule=${PLACEMENT_SCHEDULE} mesh-rows=${MESH_ROWS} mesh-cols=${MESH_COLS} arrays-per-core=${ARRAYS_PER_CORE} verify-placement"
run_stage "${OUTPUT_DIR}/08-placed.mlir" "${OUTPUT_DIR}/09-tile-deployment.mlir" \
    --sculptor-outline-tile-routines

echo "created RA-tree tile deployment: ${OUTPUT_DIR}/09-tile-deployment.mlir"
