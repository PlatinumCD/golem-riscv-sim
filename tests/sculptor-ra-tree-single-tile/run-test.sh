#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../build-scripts/common.sh
source "${TEST_DIR}/../../build-scripts/common.sh"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-ra-tree-single-tile"
readonly INPUT_MLIR="${OUTPUT_DIR}/linear.mlir"
readonly DEPLOYMENT_DIR="${OUTPUT_DIR}/deployment"
readonly OBJECT_DIR="${OUTPUT_DIR}/cores"
readonly PYTHON="${COMPILER_PYTHON}"
readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly READOBJ="${INSTALL_ROOT}/llvm/bin/llvm-readelf"
readonly NM="${INSTALL_ROOT}/llvm/bin/llvm-nm"

require_executable "${PYTHON}"
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"
require_executable "${READOBJ}"
require_executable "${NM}"
mkdir -p -- "${OUTPUT_DIR}"

PYTHONPATH="${TORCH_MLIR_PYTHON}" \
    "${PYTHON}" "${TEST_DIR}/import-model.py" "${INPUT_MLIR}"

SCULPTOR_INPUT_MLIR="${INPUT_MLIR}" \
SCULPTOR_DEPLOYMENT_DIR="${DEPLOYMENT_DIR}" \
SCULPTOR_MESH_ROWS=1 \
SCULPTOR_MESH_COLS=1 \
SCULPTOR_ARRAYS_PER_CORE=1 \
SCULPTOR_ARRAY_ROWS=8 \
SCULPTOR_ARRAY_COLS=8 \
    "${PROJECT_ROOT}/build-scripts/lower-sculptor-ra-tree.sh"

if [[ "$(grep -c '^  module @tile_0' "${DEPLOYMENT_DIR}/09-tile-deployment.mlir")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.deployment.routine_kind = "boot"' "${DEPLOYMENT_DIR}/09-tile-deployment.mlir")" -ne 1 ]]; then
    echo "RA-tree deployment did not create one tile with one boot routine" >&2
    exit 1
fi

SCULPTOR_DEPLOYMENT_MLIR="${DEPLOYMENT_DIR}/09-tile-deployment.mlir" \
SCULPTOR_CORE_OBJECT_DIR="${OBJECT_DIR}" \
SCULPTOR_CORE_LTO=none \
    "${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"

if ! "${READOBJ}" -h "${OBJECT_DIR}/core-0.o" | grep -q 'Machine:.*RISC-V'; then
    echo "RA-tree object is not a RISC-V object" >&2
    exit 1
fi
for symbol in golem_tile_core_id golem_tile_boot_tasks golem_tile_dispatch_tasks; do
    if ! "${NM}" --defined-only "${OBJECT_DIR}/core-0.o" | grep -q " ${symbol}$"; then
        echo "RA-tree object is missing ${symbol}" >&2
        exit 1
    fi
done

echo "PyTorch -> RA tree -> one outlined tile -> RISC-V runtime ABI object: PASS"
