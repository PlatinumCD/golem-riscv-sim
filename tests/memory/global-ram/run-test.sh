#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/global-ram"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" global-ram
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_GLOBAL_RAM_TILE0="${OUTPUT_DIR}/tile0.elf"
export MITTENS_GLOBAL_RAM_TILE1="${OUTPUT_DIR}/tile1.elf"

for channels in 1 2; do
    export MITTENS_GLOBAL_RAM_CHANNELS="${channels}"
    export MITTENS_GLOBAL_RAM_STATS="${OUTPUT_DIR}/channels-${channels}.csv"
    rm -f -- "${MITTENS_GLOBAL_RAM_STATS}" \
        "${OUTPUT_DIR}/channels-${channels}.log"
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" 2>&1 |
        tee "${OUTPUT_DIR}/channels-${channels}.log"
    grep -F "global RAM tile 0: PASS" \
        "${OUTPUT_DIR}/channels-${channels}.log" >/dev/null
    grep -F "global RAM tile 1: PASS" \
        "${OUTPUT_DIR}/channels-${channels}.log" >/dev/null
    grep -F "SCRATCHPAD_BOOT tile=0 " \
        "${OUTPUT_DIR}/channels-${channels}.log" >/dev/null
    grep -F "SCRATCHPAD_BOOT tile=1 " \
        "${OUTPUT_DIR}/channels-${channels}.log" >/dev/null
    if grep -F "memory_requests=" "${OUTPUT_DIR}/channels-${channels}.log" |
        grep -Ev "memory_requests=0([[:space:]]|$)" >/dev/null; then
        echo "CPU unexpectedly used a separate data-memory interface" >&2
        exit 1
    fi
done

queue_delay() {
    awk -F, '$1 == "global_ram" && $2 == "queue_delay_cycles" {print $7}' "$1"
}

one_delay="$(queue_delay "${OUTPUT_DIR}/channels-1.csv")"
two_delay="$(queue_delay "${OUTPUT_DIR}/channels-2.csv")"
if [[ -z "${one_delay}" || -z "${two_delay}" ]] ||
    ! awk -v one="${one_delay}" -v two="${two_delay}" \
        'BEGIN { exit !(one > two && one > 0) }'; then
    echo "global RAM contention did not increase queue delay: one=${one_delay}, two=${two_delay}" >&2
    exit 1
fi

if rg -U \
    'memory_region_(init_ram_from_fd|add_subregion)\([^;]*global_ram' \
    "${PROJECT_ROOT}/src/qemu/devices/mittens-sync/mittens_sync.c" \
    >/dev/null; then
    echo "global RAM is unexpectedly mapped into guest CPU address space" >&2
    exit 1
fi
rg -U 'global_ram = mmap\([^;]*MAP_SHARED' \
    "${PROJECT_ROOT}/src/qemu/devices/mittens-sync/mittens_sync.c" \
    >/dev/null

echo "global RAM: PASS (one-channel queue delay ${one_delay} cycles; two-channel ${two_delay})"
