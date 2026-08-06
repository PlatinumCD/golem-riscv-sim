#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../build-scripts/common.sh
source "${TEST_DIR}/../../build-scripts/common.sh"

readonly OUT="${BUILD_ROOT}/tests/sculptor-gpt2-vector-isolated"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"

require_executable "${SST}"
require_executable "${QEMU}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmemHierarchy.so"
"${TEST_DIR}/build-test.sh"

for version in baseline candidate; do
    trial="${OUT}/blocking-${version}-v2"
    profile="${trial}/profile"
    mkdir -p -- "${profile}"
    echo "[task 152 / blocking memory] ${version}"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_VECTOR_ELF="${OUT}/${version}.elf" \
    MITTENS_VECTOR_PROFILE="${profile}" \
    MITTENS_VECTOR_STATS="${trial}/statistics.csv" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | \
        tee "${trial}/simulation.log"
done

"${TEST_DIR}/analyze.py" \
    "${OUT}" \
    "${INSTALL_ROOT}/llvm/bin/llvm-objdump" \
    "${OUT}/result.json"
echo "isolated GPT-2 task-152 scalar/vector blocking-memory A/B: PASS"
