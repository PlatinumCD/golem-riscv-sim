#!/usr/bin/env bash
set -euo pipefail
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"
exec python3 -B "${PROJECT_ROOT}/tools/hardware/build.py" qemu -j "${BUILD_JOBS}"
