#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT_ROOT="${PROJECT_ROOT}/build/tests/memory-pc-attribution"
readonly RAW_PROFILE="${OUTPUT_ROOT}/profile-raw"
readonly REPORT="${OUTPUT_ROOT}/profile"
readonly PAIR_OUTPUT="${PROJECT_ROOT}/build/tests/deployment-runtime-pair"
readonly ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly SYMBOLIZER="${PROJECT_ROOT}/install/llvm/bin/llvm-symbolizer"

rm -rf -- "${RAW_PROFILE}" "${REPORT}"
mkdir -p -- "${RAW_PROFILE}" "${REPORT}"

MITTENS_DEPLOYMENT_MEMORY_BACKEND=memhierarchy \
MITTENS_DEPLOYMENT_MEMORY_TOPOLOGY=shared_l2 \
MITTENS_DEPLOYMENT_L2_BANKS=2 \
MITTENS_DEPLOYMENT_MEMORY_PROFILE="${RAW_PROFILE}" \
MITTENS_DEPLOYMENT_VERBOSITY=1 \
    "${PROJECT_ROOT}/tests/runtime/deployment-pair/run-test.sh"

python3 "${ANALYZER}" \
    "${RAW_PROFILE}" \
    "${REPORT}" \
    --router-statistics "${PAIR_OUTPUT}/router-statistics.csv" \
    --task-trace-directory "${RAW_PROFILE}" \
    --elf-directory "${PAIR_OUTPUT}" \
    --symbolizer "${SYMBOLIZER}"

python3 "${TEST_DIR}/validate.py" "${REPORT}/memory-sites.csv"

echo "one shared memory PC retains two caller return addresses: PASS"
