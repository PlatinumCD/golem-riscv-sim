#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly TEST_BUILD="${TEST_RESULTS_ROOT}/mesh-3x3"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"

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
export MITTENS_MESH_TILE0_ELF="${TEST_BUILD}/tile0-sender.elf"
export MITTENS_MESH_TILE8_ELF="${TEST_BUILD}/tile8-receiver.elf"

for tile_id in {1..7}; do
    export "MITTENS_MESH_TILE${tile_id}_ELF=${TEST_BUILD}/tile${tile_id}-worker.elf"
done

require_packet_route() {
    local statistics="$1"
    local router="$2"
    local statistic="$3"
    local port="$4"

    if ! awk -F, \
        -v router="${router}" \
        -v statistic="${statistic}" \
        -v port="${port}" '
            $1 == router &&
            $2 == statistic &&
            $3 == port &&
            ($7 + 0) > 0 { found = 1 }
            END { exit !found }
        ' "${statistics}"; then
        echo "mesh route did not use ${router} ${port}" >&2
        exit 1
    fi
}

for backend in mittens; do
    statistics="${TEST_BUILD}/router-statistics-${backend}.csv"
    simulation_output="${TEST_BUILD}/simulation-${backend}.log"
    export MITTENS_MESH_ROUTER_BACKEND="${backend}"
    export MITTENS_MESH_STATS="${statistics}"

    rm -f -- "${statistics}" "${simulation_output}"
    "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${simulation_output}"

    if [[ ! -s "${statistics}" ]]; then
        echo "${backend} mesh simulation did not produce router statistics" >&2
        exit 1
    fi

    qemu_launches="$(grep -c 'starting QEMU for tile ' "${simulation_output}" || true)"
    if [[ "${qemu_launches}" -ne 2 ]]; then
        echo "${backend} mesh launched ${qemu_launches} QEMUs instead of 2" >&2
        exit 1
    fi
    for tile_id in 0 8; do
        if ! grep -Eq "starting QEMU for tile ${tile_id}([^0-9]|$)" \
            "${simulation_output}"; then
            echo "${backend} mesh did not launch active tile ${tile_id}" >&2
            exit 1
        fi
    done

    statistic=flits_forwarded
    east=east
    west=west
    south=south
    north=north
    local_port=local0

    # Forward path: (0,0) -> (1,0) -> (2,0) -> (2,1) -> (2,2).
    require_packet_route "${statistics}" router_0_0 "${statistic}" "${east}"
    require_packet_route "${statistics}" router_1_0 "${statistic}" "${east}"
    require_packet_route "${statistics}" router_2_0 "${statistic}" "${south}"
    require_packet_route "${statistics}" router_2_1 "${statistic}" "${south}"
    require_packet_route "${statistics}" router_2_2 "${statistic}" "${local_port}"

    # Return path: (2,2) -> (1,2) -> (0,2) -> (0,1) -> (0,0).
    require_packet_route "${statistics}" router_2_2 "${statistic}" "${west}"
    require_packet_route "${statistics}" router_1_2 "${statistic}" "${west}"
    require_packet_route "${statistics}" router_0_2 "${statistic}" "${north}"
    require_packet_route "${statistics}" router_0_1 "${statistic}" "${north}"
    require_packet_route "${statistics}" router_0_0 "${statistic}" "${local_port}"

    echo "${backend} router-only transit proof passed: ${statistics}"
done

echo "3x3 mesh launched only endpoint QEMUs and routed through idle tiles: PASS"
