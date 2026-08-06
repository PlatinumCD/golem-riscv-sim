#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly MODEL_DIRECTORY="${BUILD_ROOT}/tests/sculptor-gpt2-8x8/model"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-gpt2-8x8/compiler-probe"
readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly LINALG_MLIR="${OUTPUT_DIR}/gpt2-linalg.mlir"
readonly CANONICAL_MLIR="${OUTPUT_DIR}/gpt2-sculptor.mlir"
readonly EXTRACTED_MLIR="${OUTPUT_DIR}/gpt2-extracted.mlir"
readonly CONVERTED_MLIR="${OUTPUT_DIR}/gpt2-converted.mlir"

"${PROJECT_ROOT}/build-scripts/build-compiler-python.sh"
require_executable "${COMPILER_PYTHON}"
require_executable "${SCULPTOR_OPT}"
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"

"${COMPILER_PYTHON}" "${TEST_DIR}/download-model.py" \
    --repository "${GPT2_MODEL_REPOSITORY}" \
    --revision "${GPT2_MODEL_REVISION}" \
    --weights-sha256 "${GPT2_MODEL_WEIGHTS_SHA256}" \
    --output "${MODEL_DIRECTORY}"

mkdir -p -- "${OUTPUT_DIR}"
layer_count="$(
    "${COMPILER_PYTHON}" -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["n_layer"])' \
        "${MODEL_DIRECTORY}/config.json"
)"
readonly EXPECTED_PROJECTION_COUNT="$((layer_count * 4 + 1))"

PYTHONPATH="${TORCH_MLIR_PYTHON}" \
    "${COMPILER_PYTHON}" \
    "${TEST_DIR}/import-model.py" \
    "${MODEL_DIRECTORY}" \
    "${LINALG_MLIR}"

"${SCULPTOR_OPT}" "${LINALG_MLIR}" \
    --sculptor-canonicalize-layers \
    -o "${CANONICAL_MLIR}"

linear_count="$(grep -c 'sculptor.nn.linear' "${CANONICAL_MLIR}")"
if [[ "${linear_count}" -ne "${EXPECTED_PROJECTION_COUNT}" ]]; then
    echo "expected ${EXPECTED_PROJECTION_COUNT} GPT-2 projections, " \
        "found ${linear_count}" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${CANONICAL_MLIR}" \
    --sculptor-extract-layers \
    -o "${EXTRACTED_MLIR}"

"${SCULPTOR_OPT}" "${EXTRACTED_MLIR}" \
    --sculptor-convert-layers \
    -o "${CONVERTED_MLIR}"

converted_count="$(grep -c ' = sculptor.mvm ' "${CONVERTED_MLIR}" || true)"
remaining_count="$(
    grep -c 'sculptor.nn.linear' "${CONVERTED_MLIR}" || true
)"

echo "Torch-MLIR import: PASS"
echo "Sculptor canonicalized projections: " \
    "${linear_count}/${EXPECTED_PROJECTION_COUNT}"
echo "Sculptor converted projections to MVM: " \
    "${converted_count}/${EXPECTED_PROJECTION_COUNT}"
echo "Sculptor unconverted projections: " \
    "${remaining_count}/${EXPECTED_PROJECTION_COUNT}"

if [[ "${converted_count}" -ne "${EXPECTED_PROJECTION_COUNT}" ||
      "${remaining_count}" -ne 0 ]]; then
    echo "Compiler boundary: GPT-2 projections operate on 128 token rows."
    echo "The generic Sculptor linear converter currently accepts only one row."
    echo "No task graph, schedule, per-core module, object, or ELF was emitted."
    exit 3
fi

echo "All ${EXPECTED_PROJECTION_COUNT} GPT-2 projections converted to MVM: PASS"
