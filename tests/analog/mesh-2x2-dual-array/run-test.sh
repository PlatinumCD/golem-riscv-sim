#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly TEST_BUILD="${BUILD_ROOT}/tests/analog-mesh-2x2-dual-array"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" \
    analog-mesh-2x2-dual-array

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}"

for tile_id in {0..3}; do
    export "MITTENS_DUAL_ARRAY_TILE${tile_id}_ELF=${TEST_BUILD}/tile${tile_id}.elf"
done

declare -A completion_times

require_packet_count() {
    local statistics="$1"
    local router="$2"
    local port="$3"
    local expected="$4"

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
        ' "${statistics}"; then
        echo "expected ${expected} packets on ${router} ${port}" >&2
        return 1
    fi
}

run_backend() {
    local backend="$1"
    local statistics="${TEST_BUILD}/router-statistics-${backend}.csv"
    local output
    output="$(mktemp)"
    trap 'rm -f -- "${output}"' RETURN

    rm -f -- "${statistics}"
    echo "running 2x2 dual-array analog mesh with ${backend} backend"
    MITTENS_ANALOG_BACKEND="${backend}" \
    MITTENS_DUAL_ARRAY_STATS="${statistics}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${output}"

    if [[ "$(grep -c 'analog output' "${output}")" -ne 8 ]]; then
        echo "${backend} run did not execute all eight analog stages" >&2
        return 1
    fi
    if ! grep -q \
        "\\[tile 0 array 1\\] final analog output .*pipeline complete" \
        "${output}"; then
        echo "${backend} run did not validate the final vector" >&2
        return 1
    fi
    if [[ ! -s "${statistics}" ]]; then
        echo "${backend} run did not produce router statistics" >&2
        return 1
    fi
    completion_times["${backend}"]="$(awk \
        '/Simulation is complete/ { print $(NF - 1), $NF }' \
        "${output}")"
    if [[ -z "${completion_times[${backend}]}" ]]; then
        echo "${backend} run did not report synchronized timing" >&2
        return 1
    fi

    # Forward pass: 0 -> 1 -> 3 -> 2.
    require_packet_count "${statistics}" router_0_0 port0 4
    require_packet_count "${statistics}" router_1_0 port2 4
    require_packet_count "${statistics}" router_1_1 port1 4

    # Reverse pass after tile 2's local array-0 -> array-1 handoff:
    # 2 -> 3 -> 1 -> 0.
    require_packet_count "${statistics}" router_0_1 port0 4
    require_packet_count "${statistics}" router_1_1 port3 4
    require_packet_count "${statistics}" router_1_0 port1 4
}

run_backend native
run_backend crosssim

if [[ "${completion_times[native]}" != \
      "${completion_times[crosssim]}" ]]; then
    echo "native and CrossSim synchronized timelines differ" >&2
    exit 1
fi

echo "2x2 dual-array analog mesh passed: 0 -> 1 -> 3 -> 2 -> 2 -> 3 -> 1 -> 0"
