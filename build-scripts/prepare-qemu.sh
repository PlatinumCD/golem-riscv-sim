#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/qemu"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/qemu"
readonly DEVICE="${PROJECT_ROOT}/components/devices/mittens-nic"
readonly BRIDGE_HEADER="${PROJECT_ROOT}/bridge/include/mittens/bridge.h"
readonly PATCH_DIR="${PROJECT_ROOT}/patches/qemu"

for command in git install; do
    require_command "${command}"
done
require_file "${DEVICE}/mittens_nic.c"
require_file "${DEVICE}/mittens_nic.h"
require_file "${BRIDGE_HEADER}"

prepare_worktree "${SUBMODULE}" "${SOURCE}" "${QEMU_COMMIT}" "QEMU"

install -D -m 0644 "${DEVICE}/mittens_nic.c" \
    "${SOURCE}/hw/misc/mittens_nic.c"
install -D -m 0644 "${DEVICE}/mittens_nic.h" \
    "${SOURCE}/include/hw/misc/mittens_nic.h"
install -D -m 0644 "${BRIDGE_HEADER}" \
    "${SOURCE}/include/mittens/bridge.h"

apply_patch_once "${SOURCE}" \
    "${PATCH_DIR}/0001-register-mittens-nic-build.patch"
apply_patch_once "${SOURCE}" \
    "${PATCH_DIR}/0002-attach-mittens-nic-to-riscv-virt.patch"

git -C "${SOURCE}" diff --check
echo "prepared QEMU source: ${SOURCE}"
