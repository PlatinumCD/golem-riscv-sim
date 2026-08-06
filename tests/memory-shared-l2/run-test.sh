#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly PROFILE_DIR="${PROJECT_ROOT}/build/tests/memory-shared-l2/profile"
readonly PAIR_OUTPUT="${PROJECT_ROOT}/build/tests/deployment-runtime-pair"

rm -rf -- "${PROFILE_DIR}"
mkdir -p -- "${PROFILE_DIR}"

MITTENS_DEPLOYMENT_MEMORY_BACKEND=memhierarchy \
MITTENS_DEPLOYMENT_MEMORY_TOPOLOGY=shared_l2 \
MITTENS_DEPLOYMENT_L2_BANKS=2 \
MITTENS_DEPLOYMENT_MEMORY_PROFILE="${PROFILE_DIR}" \
MITTENS_DEPLOYMENT_VERBOSITY=1 \
    "${PROJECT_ROOT}/tests/deployment-runtime-pair/run-test.sh"

python3 "${TEST_DIR}/validate.py" \
    "${PROFILE_DIR}" \
    "${PAIR_OUTPUT}/router-statistics.csv"

echo "two private L1 caches and two shared L2 banks: PASS"
