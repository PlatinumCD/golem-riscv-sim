#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PROJECT_ROOT}/third_party/sst-core"
readonly BUILD="${BUILD_ROOT}/sst-core"
readonly INSTALL="${INSTALL_ROOT}/sst-core"

for command in git make mpicc mpicxx python3; do
    require_command "${command}"
done
require_git_commit "${SOURCE}" "${SST_CORE_COMMIT}" "SST Core"
require_clean_submodule "${SOURCE}" "SST Core"
require_executable "${SOURCE}/configure"

export CFLAGS="${CFLAGS:--O3 -march=native -DNDEBUG}"
export CXXFLAGS="${CXXFLAGS:--O3 -march=native -DNDEBUG}"

mkdir -p -- "${BUILD}"
if [[ ! -f "${BUILD}/Makefile" ]]; then
    (
        cd -- "${BUILD}"
        "${SOURCE}/configure" \
            --prefix="${INSTALL}" \
            --disable-picky-warnings
    )
fi

make -C "${BUILD}" -j "${BUILD_JOBS}"
make -C "${BUILD}" install

require_executable "${INSTALL}/bin/sst"
require_executable "${INSTALL}/bin/sst-config"
echo "installed optimized SST Core: ${INSTALL}"
