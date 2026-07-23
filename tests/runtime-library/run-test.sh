#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly ELF="${BUILD_ROOT}/tests/runtime-library/runtime-library.elf"
readonly OUTPUT="$(mktemp)"

trap 'rm -f -- "${OUTPUT}"' EXIT

"${PROJECT_ROOT}/build-scripts/build-platform.sh" runtime-library

if [[ ! -x "${QEMU}" ]]; then
    echo "missing repository QEMU: ${QEMU}" >&2
    echo "run ${PROJECT_ROOT}/build-scripts/build-qemu.sh first" >&2
    exit 1
fi

"${QEMU}" \
    -machine virt \
    -smp 1 \
    -m 16M \
    -bios none \
    -kernel "${ELF}" \
    -display none \
    -monitor none \
    -serial stdio \
    -no-reboot 2>&1 | tee "${OUTPUT}"

if ! grep -q "Golem runtime library: PASS" "${OUTPUT}"; then
    echo "runtime library target test did not report success" >&2
    exit 1
fi
