#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly TEST_BUILD="${BUILD_ROOT}/tests/mesh-3x3"
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

"${PROJECT_ROOT}/build-scripts/build-platform.sh" mesh-3x3

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_MESH_STATS="${STATISTICS}"
export MITTENS_MESH_TILE0_ELF="${TEST_BUILD}/tile0-sender.elf"
export MITTENS_MESH_TILE8_ELF="${TEST_BUILD}/tile8-receiver.elf"

for tile_id in {1..7}; do
    export "MITTENS_MESH_TILE${tile_id}_ELF=${TEST_BUILD}/tile${tile_id}-worker.elf"
done

rm -f -- "${STATISTICS}"
"${SST}" "${TEST_DIR}/simulation.py"

if [[ ! -s "${STATISTICS}" ]]; then
    echo "mesh simulation did not produce router statistics" >&2
    exit 1
fi

require_packet_route() {
    local router="$1"
    local port="$2"

    if ! awk -F, -v router="${router}" -v port="${port}" '
        $1 == router &&
        $2 == "send_packet_count" &&
        $3 == port &&
        ($7 + 0) > 0 { found = 1 }
        END { exit !found }
    ' "${STATISTICS}"; then
        echo "mesh route did not use ${router} ${port}" >&2
        exit 1
    fi
}

# Forward path: (0,0) -> (1,0) -> (2,0) -> (2,1) -> (2,2).
require_packet_route router_0_0 port0
require_packet_route router_1_0 port0
require_packet_route router_2_0 port2
require_packet_route router_2_1 port2
require_packet_route router_2_2 port4

# Return path: (2,2) -> (1,2) -> (0,2) -> (0,1) -> (0,0).
require_packet_route router_2_2 port1
require_packet_route router_1_2 port1
require_packet_route router_0_2 port3
require_packet_route router_0_1 port3
require_packet_route router_0_0 port4

echo "3x3 mesh proof passed; router statistics: ${STATISTICS}"
