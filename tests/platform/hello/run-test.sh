#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly ELF="${TEST_RESULTS_ROOT}/hello/hello.elf"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" hello

if [[ ! -x "${QEMU}" ]]; then
    echo "missing repository QEMU: ${QEMU}" >&2
    echo "run ${PROJECT_ROOT}/build-scripts/build-qemu.sh first" >&2
    exit 1
fi

export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${ELF}"
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
exec "${INSTALL_ROOT}/sst-core/bin/sst" "${PROJECT_ROOT}/src/sst/tests/qemu_boot.py"
