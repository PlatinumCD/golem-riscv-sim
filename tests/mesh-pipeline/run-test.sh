#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly TEST_BUILD="${BUILD_ROOT}/tests/mesh-pipeline"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly STATISTICS="${TEST_BUILD}/router-statistics.csv"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" mesh-pipeline

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_PIPELINE_STATS="${STATISTICS}"
export MITTENS_PIPELINE_TILE0_ELF="${TEST_BUILD}/tile0-dispatcher.elf"

for tile_id in {1..8}; do
    export "MITTENS_PIPELINE_TILE${tile_id}_ELF=${TEST_BUILD}/tile${tile_id}-worker.elf"
done

rm -f -- "${STATISTICS}"
"${SST}" "${TEST_DIR}/simulation.py"

if [[ ! -s "${STATISTICS}" ]]; then
    echo "pipeline simulation did not produce router statistics" >&2
    exit 1
fi

require_packet_count() {
    local router="$1"
    local port="$2"
    local expected="$3"

    if ! awk -F, \
        -v router="${router}" \
        -v port="${port}" \
        -v expected="${expected}" '
            $1 == router &&
            $2 == "send_packet_count" &&
            $3 == port {
                found = 1
                total += $7
            }
            END { exit !(found && total == expected) }
        ' "${STATISTICS}"; then
        echo "expected ${expected} packet on ${router} ${port}" >&2
        exit 1
    fi
}

# One-hop serpentine computation stages.
require_packet_count router_0_0 port0 1
require_packet_count router_1_0 port0 1
require_packet_count router_2_0 port2 1
require_packet_count router_2_1 port1 1
require_packet_count router_1_1 port1 1
require_packet_count router_0_1 port2 1
require_packet_count router_0_2 port0 1
require_packet_count router_1_2 port0 1

# Four-hop result return from tile 8 to tile 0.
require_packet_count router_2_2 port1 1
require_packet_count router_1_2 port1 1
require_packet_count router_0_2 port3 1
require_packet_count router_0_1 port3 1
require_packet_count router_0_0 port4 1

echo "3x3 computation pipeline passed; final float32 result: 36.0"
echo "router statistics: ${STATISTICS}"
