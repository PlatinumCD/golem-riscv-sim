#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly TEST_BUILD="${TEST_RESULTS_ROOT}/analog-route-2x2"
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

"${PROJECT_ROOT}/build-scripts/build-platform.sh" analog-route-2x2

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}"

declare -A completion_times
declare -A completion_nanoseconds
declare -A physical_word_hops

time_to_nanoseconds() {
    local value="$1"
    local unit="$2"
    local factor

    case "${unit}" in
        ps) factor=0.001 ;;
        ns) factor=1 ;;
        us) factor=1000 ;;
        ms) factor=1000000 ;;
        s) factor=1000000000 ;;
        *)
            echo "unsupported SST time unit: ${unit}" >&2
            return 1
            ;;
    esac

    awk -v value="${value}" -v factor="${factor}" \
        'BEGIN { printf "%.3f", value * factor }'
}

run_route() {
    local route="$1"
    local expected_word_hops="$2"
    local statistics="${TEST_BUILD}/${route}/router-statistics.csv"
    local output
    local tile_id
    local value
    local unit

    output="$(mktemp)"
    trap 'rm -f -- "${output}"' RETURN

    for tile_id in {0..3}; do
        export "MITTENS_ANALOG_ROUTE_TILE${tile_id}_ELF=${TEST_BUILD}/${route}/tile${tile_id}.elf"
    done
    export MITTENS_ANALOG_ROUTE_STATS="${statistics}"

    rm -f -- "${statistics}"
    echo "running analog route ${route}"
    "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${output}"

    if ! grep -q "ANALOG_ROUTE_${route}_PASS" "${output}"; then
        echo "route ${route} did not validate its final vector" >&2
        return 1
    fi
    if [[ ! -s "${statistics}" ]]; then
        echo "route ${route} did not produce router statistics" >&2
        return 1
    fi

    completion_times["${route}"]="$(awk \
        '/Simulation is complete/ { print $(NF - 1), $NF }' \
        "${output}")"
    read -r value unit <<< "${completion_times[${route}]}"
    completion_nanoseconds["${route}"]="$(time_to_nanoseconds \
        "${value}" "${unit}")"

    physical_word_hops["${route}"]="$(awk -F, '
        $2 == "send_packet_count" &&
        ($3 == "port0" || $3 == "port1" ||
         $3 == "port2" || $3 == "port3") {
            total += $7
        }
        END { print total + 0 }
    ' "${statistics}")"

    if [[ "${physical_word_hops[${route}]}" -ne \
          "${expected_word_hops}" ]]; then
        echo "route ${route} used ${physical_word_hops[${route}]} physical word-hops; expected ${expected_word_hops}" >&2
        return 1
    fi
}

run_route 0132 12
run_route 0312 20

if ! awk \
    -v direct="${completion_nanoseconds[0132]}" \
    -v diagonal="${completion_nanoseconds[0312]}" \
    'BEGIN { exit !(diagonal > direct) }'; then
    echo "route 0312 was expected to take longer than route 0132" >&2
    exit 1
fi

difference="$(awk \
    -v direct="${completion_nanoseconds[0132]}" \
    -v diagonal="${completion_nanoseconds[0312]}" \
    'BEGIN { printf "%.3f", diagonal - direct }')"

echo "route 0132: ${completion_times[0132]}, ${physical_word_hops[0132]} physical 32-bit word-hops"
echo "route 0312: ${completion_times[0312]}, ${physical_word_hops[0312]} physical 32-bit word-hops"
echo "route 0312 is ${difference} ns slower than route 0132"
