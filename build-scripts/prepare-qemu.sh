#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/qemu"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/qemu"
readonly DEVICE="${PROJECT_ROOT}/components/devices/mittens-nic"
readonly ANALOG_DEVICE="${PROJECT_ROOT}/components/devices/mittens-analog"
readonly SYNC_DEVICE="${PROJECT_ROOT}/components/devices/mittens-sync"
readonly GOLEM_ANALOG="${PROJECT_ROOT}/components/qemu/golem-analog"
readonly BRIDGE_HEADER="${PROJECT_ROOT}/bridge/include/mittens/NICTileBridge.h"
readonly ANALOG_BRIDGE_HEADER="${PROJECT_ROOT}/bridge/include/mittens/AnalogTileBridge.h"
readonly SYNC_BRIDGE_HEADER="${PROJECT_ROOT}/bridge/include/mittens/SyncTileBridge.h"
readonly PATCH_DIR="${PROJECT_ROOT}/patches/qemu"

for command in git install rg; do
    require_command "${command}"
done
require_file "${DEVICE}/mittens_nic.c"
require_file "${DEVICE}/mittens_nic.h"
require_file "${ANALOG_DEVICE}/mittens_analog.c"
require_file "${ANALOG_DEVICE}/mittens_analog.h"
require_file "${SYNC_DEVICE}/mittens_sync.c"
require_file "${SYNC_DEVICE}/mittens_sync.h"
require_file "${GOLEM_ANALOG}/golem_analog_helper.c"
require_file "${GOLEM_ANALOG}/trans_golem_analog.c.inc"
require_file "${BRIDGE_HEADER}"
require_file "${ANALOG_BRIDGE_HEADER}"
require_file "${SYNC_BRIDGE_HEADER}"

prepare_worktree "${SUBMODULE}" "${SOURCE}" "${QEMU_COMMIT}" "QEMU"

apply_qemu_patch_once() {
    local patch_file="$1"
    local sentinel="$2"

    if rg --fixed-strings --quiet "${sentinel}" "${SOURCE}"; then
        return 0
    fi
    apply_patch_once "${SOURCE}" "${patch_file}"
}

install -D -m 0644 "${DEVICE}/mittens_nic.c" \
    "${SOURCE}/hw/misc/mittens_nic.c"
install -D -m 0644 "${DEVICE}/mittens_nic.h" \
    "${SOURCE}/include/hw/misc/mittens_nic.h"
install -D -m 0644 "${BRIDGE_HEADER}" \
    "${SOURCE}/include/mittens/NICTileBridge.h"
install -D -m 0644 "${ANALOG_DEVICE}/mittens_analog.c" \
    "${SOURCE}/hw/misc/mittens_analog.c"
install -D -m 0644 "${ANALOG_DEVICE}/mittens_analog.h" \
    "${SOURCE}/include/hw/misc/mittens_analog.h"
install -D -m 0644 "${SYNC_DEVICE}/mittens_sync.c" \
    "${SOURCE}/hw/misc/mittens_sync.c"
install -D -m 0644 "${SYNC_DEVICE}/mittens_sync.h" \
    "${SOURCE}/include/hw/misc/mittens_sync.h"
install -D -m 0644 "${GOLEM_ANALOG}/golem_analog_helper.c" \
    "${SOURCE}/target/riscv/golem_analog_helper.c"
install -D -m 0644 "${GOLEM_ANALOG}/trans_golem_analog.c.inc" \
    "${SOURCE}/target/riscv/insn_trans/trans_golem_analog.c.inc"
install -D -m 0644 "${ANALOG_BRIDGE_HEADER}" \
    "${SOURCE}/include/mittens/AnalogTileBridge.h"
install -D -m 0644 "${SYNC_BRIDGE_HEADER}" \
    "${SOURCE}/include/mittens/SyncTileBridge.h"

apply_qemu_patch_once \
    "${PATCH_DIR}/0001-register-mittens-nic-build.patch" \
    "files('mittens_analog.c')"
apply_qemu_patch_once \
    "${PATCH_DIR}/0002-attach-mittens-nic-to-riscv-virt.patch" \
    "mittens_analog_create();"
apply_qemu_patch_once \
    "${PATCH_DIR}/0004-decode-golem-analog-instructions.patch" \
    "DEF_HELPER_4(golem_analog"
apply_qemu_patch_once \
    "${PATCH_DIR}/0005-synchronize-tcg-with-sst.patch" \
    "mittens_sync_account_icount("
apply_qemu_patch_once \
    "${PATCH_DIR}/0006-synchronize-guest-exit.patch" \
    "mittens_sync_guest_exit();"

git -C "${SOURCE}" diff --check
echo "prepared QEMU source: ${SOURCE}"
