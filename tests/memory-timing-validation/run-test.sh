#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_ROOT="${BUILD_ROOT}/tests/memory-timing-validation"
readonly RESULTS="${OUTPUT_ROOT}/results.csv"
readonly ANALYZER="${TEST_DIR}/analyze-results.py"

require_executable "${SST}"
require_executable "${QEMU}"
require_executable "${ANALYZER}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmemHierarchy.so"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" memory-timing-validation

case_specs=()
for name in conflict capacity; do
    trial="${OUTPUT_ROOT}/${name}"
    profile="${trial}/profile"
    log="${trial}/simulation.log"
    elf="${OUTPUT_ROOT}/memory-${name}.elf"

    require_file "${elf}"
    mkdir -p -- "${profile}"
    rm -f -- "${profile}/tile-0-memory.csv" \
        "${profile}/tile-0-summary.csv" \
        "${log}"

    echo "[private L1 timing] case=${name}"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_TEST_ELF="${elf}" \
    MITTENS_MEMORY_PROFILE="${profile}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${log}"

    grep -q "QEMU tile 0 exited with exit status 0" "${log}"
    require_file "${profile}/tile-0-memory.csv"
    require_file "${profile}/tile-0-summary.csv"
    case_specs+=(
        "${name}:${profile}/tile-0-memory.csv:${profile}/tile-0-summary.csv:${log}"
    )
done

"${ANALYZER}" "${RESULTS}" "${case_specs[@]}"
echo "private-L1 hit, miss, conflict, eviction, write, and capacity timing: PASS"
