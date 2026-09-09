#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/qemu"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/qemu"
readonly DEVICE="${QEMU_DEVICE_ROOT}/mittens-nic"
readonly ANALOG_DEVICE="${QEMU_DEVICE_ROOT}/mittens-analog"
readonly SYNC_DEVICE="${QEMU_DEVICE_ROOT}/mittens-sync"
readonly GOLEM_ANALOG="${QEMU_INSTRUCTION_ROOT}/golem-analog"
readonly BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/NICTileBridge.h"
readonly ANALOG_BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/AnalogTileBridge.h"
readonly SYNC_BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/SyncTileBridge.h"
readonly PATCH_DIR="${HARDWARE_ROOT}/patches/qemu"
require_owned_comparison_output "${SOURCE}" "${PREPARED_SOURCE_ROOT}"

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
if [[ "${HARDWARE_TREE}" == src ]]; then
    install -D -m 0644 "${HARDWARE_ROOT}/bridge/include/mittens/MemoryMap.h" \
        "${SOURCE}/include/mittens/MemoryMap.h"
fi

apply_qemu_patch_once \
    "${PATCH_DIR}/0001-register-mittens-nic-build.patch" \
    "files('mittens_sync.c')"
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
    "${PATCH_DIR}/0013-instantiate-mittens-sync.patch" \
    "mittens_sync_create();"
apply_qemu_patch_once \
    "${PATCH_DIR}/0014-enable-mittens-icount-hooks.patch" \
    "cpu_budget = mittens_sync_wait_for_grant(cpu_budget);"
apply_qemu_patch_once \
    "${PATCH_DIR}/0015-begin-mittens-icount-quantum.patch" \
    "mittens_sync_begin_quantum(cpu->icount_budget);"
apply_qemu_patch_once \
    "${PATCH_DIR}/0006-synchronize-guest-exit.patch" \
    "mittens_sync_guest_exit();"
apply_qemu_patch_once \
    "${PATCH_DIR}/0007-count-riscv-vector-instructions.patch" \
    "is_mittens_vector_instruction"
apply_qemu_patch_once \
    "${PATCH_DIR}/0008-time-riscv-data-memory.patch" \
    "The memHierarchy backend is timing-only"
apply_qemu_patch_once \
    "${PATCH_DIR}/0009-count-riscv-vset-instructions.patch" \
    "Mittens counts vset before its mandatory TB exit"
apply_qemu_patch_once \
    "${PATCH_DIR}/0010-attribute-memory-accesses-to-guest-pc.patch" \
    "mittens_sync_guest_pc"
apply_qemu_patch_once \
    "${PATCH_DIR}/0011-synchronize-riscv-memory-fence.patch" \
    "gen_helper_mittens_sync_memory_fence"
apply_qemu_patch_once \
    "${PATCH_DIR}/0012-report-riscv-load-register-dependencies.patch" \
    "gen_helper_mittens_sync_memory_instruction"
apply_qemu_patch_once \
    "${PATCH_DIR}/0016-precise-mittens-memory-boundaries.patch" \
    "is_mittens_memory_instruction"
apply_qemu_patch_once \
    "${PATCH_DIR}/0017-count-vector-memory-before-yield.patch" \
    "Count the issued vector memory instruction before its element"
apply_qemu_patch_once \
    "${PATCH_DIR}/0018-precise-fence-and-compressed-boundaries.patch" \
    "End the TB at every yielding instruction, including FENCE."

git -C "${SOURCE}" diff --check
echo "prepared QEMU source: ${SOURCE}"
