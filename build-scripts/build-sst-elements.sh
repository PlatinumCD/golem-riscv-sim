#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

# The canonical builder installs Mittens without registering into shared SST Core.
exec python3 "${PROJECT_ROOT}/tools/hardware/build.py" sst -j "${BUILD_JOBS}"
