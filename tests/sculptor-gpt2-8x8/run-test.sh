#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly MODEL_DIRECTORY="${BUILD_ROOT}/tests/sculptor-gpt2-8x8/model"

"${PROJECT_ROOT}/build-scripts/build-compiler-python.sh"
require_executable "${COMPILER_PYTHON}"

"${COMPILER_PYTHON}" "${TEST_DIR}/download-model.py" \
    --repository "${GPT2_MODEL_REPOSITORY}" \
    --revision "${GPT2_MODEL_REVISION}" \
    --weights-sha256 "${GPT2_MODEL_WEIGHTS_SHA256}" \
    --output "${MODEL_DIRECTORY}"

"${COMPILER_PYTHON}" "${TEST_DIR}/load-model.py" \
    "${MODEL_DIRECTORY}"
