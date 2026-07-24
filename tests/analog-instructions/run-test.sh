#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly CROSSSIM_SITE_PACKAGES="${INSTALL_ROOT}/cross-sim/python"
readonly ELF="${BUILD_ROOT}/tests/analog-instructions/analog-instructions.elf"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" analog-instructions

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${ELF}"
export PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}"

run_backend() {
    local backend="$1"
    local output
    output="$(mktemp)"
    trap 'rm -f -- "${output}"' RETURN

    echo "running analog instruction test with ${backend} backend"
    MITTENS_ANALOG_BACKEND="${backend}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${output}"

    if ! grep -q \
        "ANALOG_INSTRUCTION_PASS: two asynchronous arrays and all five instructions" \
        "${output}"; then
        echo "${backend} analog instruction test did not report success" >&2
        return 1
    fi
}

run_backend native
run_backend crosssim

echo "analog custom-instruction end-to-end test passed"
