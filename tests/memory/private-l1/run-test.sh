#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly ELF="${BUILD_ROOT}/tests/memory-hierarchy-l1/memory-hierarchy-l1.elf"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" memory-hierarchy-l1

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${ELF}"

run_backend() {
    local backend="$1"
    local init_batching="${2:-0}"
    local output
    output="$(mktemp)"
    trap 'rm -f -- "${output}"' RETURN

    echo "running one-tile memory test with ${backend} backend (init batching=${init_batching})"
    MITTENS_MEMORY_BACKEND="${backend}" \
        MITTENS_MEMORY_INIT_BATCHING="${init_batching}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${output}"

    grep -q "MEMORY_HIERARCHY_L1_PASS" "${output}"
    if [[ "${backend}" == "native" ]]; then
        grep -q "memory_requests=0 memory_responses=0" "${output}"
    else
        grep -Eq "memory_requests=[1-9][0-9]*" "${output}"
        grep -Eq "tile0\\.private_l1\\.CacheHits.*Sum\\.u64 = [1-9]" \
            "${output}"
        grep -Eq "tile0\\.private_l1\\.CacheMisses.*Sum\\.u64 = [1-9]" \
            "${output}"
        if [[ "${init_batching}" == "1" ]]; then
            grep -Eq "stop_memory_init=1 .*memory_init_handshakes=1 " \
                "${output}"
            grep -Eq "memory_init_accesses=[1-9][0-9]* " \
                "${output}"
            grep -Eq "memory_init_cycles=[1-9][0-9]* " \
                "${output}"
        else
            grep -q "stop_memory_init=0" "${output}"
            grep -q "memory_init_handshakes=0" "${output}"
        fi
    fi
}

run_backend native
run_backend memhierarchy
run_backend memhierarchy 1

echo "native, private-L1, and one-handshake initialization backends passed"
