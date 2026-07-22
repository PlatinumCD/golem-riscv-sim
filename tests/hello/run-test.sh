#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
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
    -smp 1 \
    -m 16M \
    -bios none \
    -kernel "${ELF}" \
    -display none \
    -monitor none \
    -serial stdio \
    -no-reboot
