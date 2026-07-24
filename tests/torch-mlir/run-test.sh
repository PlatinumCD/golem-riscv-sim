#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly TORCH_MLIR_OPT="${INSTALL_ROOT}/torch-mlir/bin/torch-mlir-opt"
readonly INPUT="${PROJECT_ROOT}/third_party/torch-mlir/test/Conversion/TorchToLinalg/basic.mlir"
readonly OUTPUT="$(mktemp)"

trap 'rm -f -- "${OUTPUT}"' EXIT

if [[ ! -x "${TORCH_MLIR_OPT}" ]]; then
    echo "missing Torch-MLIR compiler: ${TORCH_MLIR_OPT}" >&2
    echo "run ${PROJECT_ROOT}/bootstrap.sh torch-mlir first" >&2
    exit 1
fi
if [[ ! -f "${INPUT}" ]]; then
    echo "missing Torch-to-Linalg test input: ${INPUT}" >&2
    exit 1
fi

"${TORCH_MLIR_OPT}" \
    "${INPUT}" \
    -convert-torch-to-linalg \
    -split-input-file \
    -verify-diagnostics \
    -o "${OUTPUT}"

if ! grep -q 'linalg.matmul' "${OUTPUT}"; then
    echo "Torch-to-Linalg output does not contain linalg.matmul" >&2
    exit 1
fi

echo "Torch-MLIR compiler: PASS"
