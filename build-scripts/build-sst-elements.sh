#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PREPARED_SOURCE_ROOT}/sst-elements"
readonly BUILD="${BUILD_ROOT}/sst-elements"
readonly INSTALL="${INSTALL_ROOT}/sst-elements"
readonly CORE_INSTALL="${INSTALL_ROOT}/sst-core"

for command in autoconf automake autoreconf libtoolize make python3; do
    require_command "${command}"
done
require_executable "${CORE_INSTALL}/bin/sst-config"
"${SCRIPT_DIR}/prepare-sst-elements.sh"

export CFLAGS="${CFLAGS:--O3 -march=native -DNDEBUG}"
export CXXFLAGS="${CXXFLAGS:--O3 -march=native -DNDEBUG}"

(
    cd -- "${SOURCE}"
    ./autogen.sh
)

mkdir -p -- "${BUILD}"
(
    cd -- "${BUILD}"
    "${SOURCE}/configure" \
        --prefix="${INSTALL}" \
        --with-sst-core="${CORE_INSTALL}" \
        --disable-picky-warnings
)

make -C "${BUILD}" -j "${BUILD_JOBS}"
make -C "${BUILD}" install

require_file "${INSTALL}/lib/sst-elements-library/libmittens.so"
require_file "${INSTALL}/lib/sst-elements-library/libmerlin.so"
echo "installed SST Elements (Merlin and Mittens): ${INSTALL}"
