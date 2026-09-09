#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly TEST_PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
export GOLEM_HARDWARE_TREE=src
source "${TEST_PROJECT_ROOT}/build-scripts/common.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${MITTENS_TEST_ELEMENT_LIBRARY:-${INSTALL_ROOT}/sst-elements/lib/sst-elements-library}"
readonly SIMULATION="${TEST_DIR}/qemu_ready_set_boot.py"
readonly PROOF_FILE="${TEST_DIR}/qemu_ready_set_boot_proof.json"
readonly GUEST_DIR="${MITTENS_READY_SET_GUEST_DIRECTORY:-${BUILD_ROOT}/tests/qemu-ready-set}"
readonly ELF="${GUEST_DIR}/qemu-ready-set.elf"
readonly ANALOG_ELF="${GUEST_DIR}/qemu-ready-set-analog.elf"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing ready-set test executable: ${executable}" >&2
        exit 1
    fi
done
for path in "${ELEMENT_LIBRARY}/libmittens.so" "${SIMULATION}" "${PROOF_FILE}"; do
    if [[ ! -f "${path}" ]]; then
        echo "missing ready-set test input: ${path}" >&2
        exit 1
    fi
done

if [[ -n "${MITTENS_READY_SET_GUEST_DIRECTORY:-}" ]]; then
    # Explicit hardware-only replay of immutable guest binaries. The default
    # still rebuilds guests; this is not a substitute for fresh-build acceptance.
    echo "using frozen ready-set guests: ${GUEST_DIR}"
else
    "${PROJECT_ROOT}/build-scripts/build-platform.sh" qemu-ready-set
fi
for elf in "${ELF}" "${ANALOG_ELF}"; do
    if [[ ! -f "${elf}" ]]; then
        echo "ready-set test ELF was not built: ${elf}" >&2
        exit 1
    fi
done

mkdir -p -- "${BUILD_ROOT}/tests"
readonly RESULT_ROOT="$(mktemp -d "${BUILD_ROOT}/tests/qemu-ready-set-sst.XXXXXX")"
sha256sum -- "${ELF}" "${ANALOG_ELF}" >"${RESULT_ROOT}/guest-sha256.txt"
readonly PROOF="$(sha256sum -- "${PROOF_FILE}" | cut -d' ' -f1)"
export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"

run_point() {
    local mode="$1"
    local tiles="$2"
    local workers="$3"
    local initialization_quantum="$4"
    local output="${RESULT_ROOT}/${mode}-t${tiles}-w${workers}"
    local elf="${ELF}"
    local analog_first=0

    if [[ "${mode}" == analog ]]; then
        elf="${ANALOG_ELF}"
        analog_first=1
    fi

    mkdir -p -- "${output}"
    MITTENS_READY_SET_TILES="${tiles}" \
    MITTENS_READY_SET_WORKERS="${workers}" \
    MITTENS_READY_SET_RUNTIME="$([[ "${workers}" -gt 1 ]] && printf 1 || printf 0)" \
    MITTENS_READY_SET_QEMU="${QEMU}" \
    MITTENS_READY_SET_ELF="${elf}" \
    MITTENS_READY_SET_OUTPUT="${output}" \
    MITTENS_READY_SET_PROOF="${PROOF}" \
    MITTENS_READY_SET_ANALOG_FIRST="${analog_first}" \
    MITTENS_READY_SET_INITIALIZATION_QUANTUM="${initialization_quantum}" \
    MITTENS_READY_SET_RUNTIME_QUANTUM=4096 \
        /usr/bin/time -q -f '%e' -o "${output}/wall-seconds.txt" \
        "${SST}" "${SIMULATION}" >"${output}/simulation.log" 2>&1
}

verify_pair() {
    local mode="$1"
    local tiles="$2"
    local workers="$3"
    local serial="${RESULT_ROOT}/${mode}-t${tiles}-w1"
    local parallel="${RESULT_ROOT}/${mode}-t${tiles}-w${workers}"
    local tile

    if [[ "$(find "${serial}/uart" -type f -name 'tile-*.log' | wc -l)" -ne "${tiles}" ||
          "$(find "${parallel}/uart" -type f -name 'tile-*.log' | wc -l)" -ne "${tiles}" ||
          "$(find "${serial}/performance" -type f -name 'tile-*-summary.csv' | wc -l)" -ne "${tiles}" ||
          "$(find "${parallel}/performance" -type f -name 'tile-*-summary.csv' | wc -l)" -ne "${tiles}" ]]; then
        echo "ready-set ${mode} ${tiles}-tile run produced incomplete evidence" >&2
        exit 1
    fi

    for ((tile = 0; tile < tiles; ++tile)); do
        if [[ "$(grep -c '^MITTENS_READY_SET_PASS' "${serial}/uart/tile-${tile}.log")" -ne 1 ||
              "$(grep -c '^MITTENS_READY_SET_PASS' "${parallel}/uart/tile-${tile}.log")" -ne 1 ]]; then
            echo "ready-set ${mode} tile ${tile} did not pass exactly once" >&2
            exit 1
        fi
        cmp "${serial}/uart/tile-${tile}.log" \
            "${parallel}/uart/tile-${tile}.log"
        cmp "${serial}/performance/tile-${tile}-summary.csv" \
            "${parallel}/performance/tile-${tile}-summary.csv"
        if [[ "${mode}" == marker ]] &&
           ! awk -F, '$1 == "stop_memory_fence" && $2 > 0 { found = 1 }
                      END { exit found ? 0 : 1 }' \
               "${parallel}/performance/tile-${tile}-summary.csv"; then
            echo "ready-set marker tile ${tile} did not exercise a memory fence" >&2
            exit 1
        fi
        if [[ "${mode}" == analog ]] &&
           ! awk -F, '$1 == "stop_analog_submit" && $2 > 0 { found = 1 }
                      END { exit found ? 0 : 1 }' \
               "${parallel}/performance/tile-${tile}-summary.csv"; then
            echo "ready-set analog tile ${tile} did not exercise an analog submit" >&2
            exit 1
        fi
    done

    if grep -Eiq 'fatal|panic|assert|MITTENS_READY_SET_ERROR' \
        "${serial}/simulation.log" "${parallel}/simulation.log" \
        "${serial}"/uart/tile-*.log "${parallel}"/uart/tile-*.log; then
        echo "ready-set ${mode} ${tiles}-tile run reported an error" >&2
        exit 1
    fi

    printf 'ready_set_sst mode=%s tiles=%s workers=%s serial_wall=%s parallel_wall=%s identity=PASS\n' \
        "${mode}" "${tiles}" "${workers}" \
        "$(<"${serial}/wall-seconds.txt")" \
        "$(<"${parallel}/wall-seconds.txt")"
}

for mode in marker quantum analog; do
    case "${mode}" in
        marker) initialization_quantum=67108864 ;;
        quantum) initialization_quantum=64 ;;
        analog) initialization_quantum=67108864 ;;
    esac
    for tiles in 2 4 16; do
        if [[ "${tiles}" -le 4 ]]; then
            workers="${tiles}"
        else
            workers=10
        fi
        run_point "${mode}" "${tiles}" 1 "${initialization_quantum}"
        run_point "${mode}" "${tiles}" "${workers}" "${initialization_quantum}"
        verify_pair "${mode}" "${tiles}" "${workers}"
    done
done

printf 'qemu_ready_set_sst_results=%s\n' "${RESULT_ROOT}"
sha256sum --check "${RESULT_ROOT}/guest-sha256.txt"
printf 'qemu ready-set focused SST tests: PASS\n'
