#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly ELF="${BUILD_ROOT}/tests/analog-ops/analog-ops.elf"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" analog-ops

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${ELF}"
export PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}"

declare -A sync_summaries
declare -A completion_times

run_backend() {
    local backend="$1"
    local output
    output="$(mktemp)"
    trap 'rm -f -- "${output}"' RETURN

    echo "running single-tile analog operation test with ${backend} backend"
    MITTENS_ANALOG_BACKEND="${backend}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${output}"

    if [[ "$(grep -c ': PASS' "${output}")" -ne 5 ]]; then
        echo "${backend} test did not report all five operation checks" >&2
        return 1
    fi
    if ! grep -q \
        "ANALOG_OPS_PASS: matrix-vector output validated" \
        "${output}"; then
        echo "${backend} single-tile analog operation test failed" >&2
        return 1
    fi

    sync_summaries["${backend}"]="$(sed -n \
        's/.*fd 41 synchronized //p' "${output}")"
    completion_times["${backend}"]="$(awk \
        '/Simulation is complete/ { print $(NF - 1), $NF }' \
        "${output}")"
    if [[ -z "${sync_summaries[${backend}]}" ||
          -z "${completion_times[${backend}]}" ]]; then
        echo "${backend} run did not report synchronized timing" >&2
        return 1
    fi
}

run_backend native
run_backend crosssim

if [[ "${sync_summaries[native]}" != "${sync_summaries[crosssim]}" ||
      "${completion_times[native]}" != "${completion_times[crosssim]}" ]]; then
    echo "native and CrossSim synchronized timelines differ" >&2
    exit 1
fi

echo "single-tile analog operation/output test passed"
