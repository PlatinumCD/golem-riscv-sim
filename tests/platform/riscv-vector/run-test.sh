#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly ELF="${TEST_RESULTS_ROOT}/riscv-vector/riscv-vector.elf"

for executable in "${QEMU}" "${SST}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" riscv-vector

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${ELF}"

output="$("${SST}" "${TEST_DIR}/simulation.py" 2>&1)"
printf '%s\n' "${output}"

if ! grep -q \
    "RISCV_VECTOR_PASS: RVV 1.0 VLEN=256 floating-point vector add" \
    <<< "${output}"; then
    echo "RISC-V vector test did not report success" >&2
    exit 1
fi

echo "QEMU RISC-V vector execution: PASS"
