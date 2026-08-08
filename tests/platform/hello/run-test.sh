#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly ELF="${BUILD_ROOT}/tests/hello/hello.elf"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" hello

if [[ ! -x "${QEMU}" ]]; then
    echo "missing repository QEMU: ${QEMU}" >&2
    echo "run ${PROJECT_ROOT}/build-scripts/build-qemu.sh first" >&2
    exit 1
fi

exec "${QEMU}" \
    -machine virt \
    -cpu "${QEMU_RISCV_CPU}" \
    -smp 1 \
    -m 16M \
    -bios none \
    -kernel "${ELF}" \
    -display none \
    -monitor none \
    -serial stdio \
    -no-reboot
