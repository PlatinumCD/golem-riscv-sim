#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

if [[ "${GOLEM_BUILD_SCOPE:-hardware}" == shared ]]; then
    readonly SOURCE="${PREPARED_SOURCE_ROOT}/sst-elements"
    readonly BUILD="${BUILD_ROOT}/sst-elements"
    readonly INSTALL="${INSTALL_ROOT}/sst-elements"
    require_owned_comparison_output "${BUILD}" "${BUILD_ROOT}"
    require_owned_comparison_output "${INSTALL}" "${INSTALL_ROOT}"
    require_executable "${INSTALL_ROOT}/sst-core/bin/sst-config"
    "${SCRIPT_DIR}/prepare-sst-elements.sh"
    (cd -- "${SOURCE}" && ./autogen.sh)
    mkdir -p -- "${BUILD}"
    (cd -- "${BUILD}" && "${SOURCE}/configure" --prefix="${INSTALL}" \
        --with-sst-core="${INSTALL_ROOT}/sst-core" --disable-picky-warnings)
    make -C "${BUILD}" -j "${BUILD_JOBS}"
    make -C "${BUILD}" install
    require_file "${INSTALL}/lib/sst-elements-library/libmerlin.so"
    require_file "${INSTALL}/lib/sst-elements-library/libmemHierarchy.so"
    exit 0
fi

# The canonical builder installs Mittens without registering into shared SST Core.
exec python3 "${PROJECT_ROOT}/tools/hardware/build.py" sst -j "${BUILD_JOBS}"
