#!/usr/bin/env bash
# Run a command against the selected hardware; never changes the caller's env.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${repo}/build-scripts/common.sh"
export GOLEM_RESOLVED_CONFIG_DIR="${GOLEM_RESOLVED_CONFIG_DIR:-${GOLEM_BUILD_ROOT}/resolved-configurations}"
export SST_LIB_PATH="${GOLEM_INSTALL_ROOT}/lib:${PROJECT_ROOT}/install/sst-elements/lib/sst-elements-library"
export QEMU_SYSTEM_RISCV64="${GOLEM_INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_QEMU="${QEMU_SYSTEM_RISCV64}"
export PATH="${GOLEM_INSTALL_ROOT}/bin:${PATH}"
if [[ "${1:-}" == --profile ]]; then
    if (($# < 3)) || [[ -z "$2" ]]; then
        echo 'usage: env.sh --profile OUTPUT_DIRECTORY COMMAND [ARG ...]' >&2
        exit 2
    fi
    export TILE_CYCLE_PROFILE=1
    export TILE_CYCLE_PROFILE_DIRECTORY="$2"
    shift 2
elif [[ "${1:-}" == --no-profile ]]; then
    export TILE_CYCLE_PROFILE=0
    unset TILE_CYCLE_PROFILE_DIRECTORY
    shift
fi
if (($# == 0)); then
    echo 'usage: bash tools/hardware/env.sh [--profile OUTPUT_DIRECTORY | --no-profile] COMMAND [ARG ...]' >&2
    exit 2
fi
exec "$@"
