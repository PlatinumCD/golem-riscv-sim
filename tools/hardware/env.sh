#!/usr/bin/env bash
# Run a command against the selected hardware; never changes the caller's env.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${repo}/build-scripts/common.sh"
export GOLEM_RESOLVED_CONFIG_DIR="${GOLEM_RESOLVED_CONFIG_DIR:-${GOLEM_BUILD_ROOT}/resolved-configurations}"
export SST_LIB_PATH="${GOLEM_INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
export QEMU_SYSTEM_RISCV64="${GOLEM_INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_QEMU="${QEMU_SYSTEM_RISCV64}"
export PATH="${GOLEM_INSTALL_ROOT}/sst-core/bin:${PATH}"
if (($# == 0)); then
    echo 'usage: bash tools/hardware/env.sh COMMAND [ARG ...]' >&2
    exit 2
fi
exec "$@"
