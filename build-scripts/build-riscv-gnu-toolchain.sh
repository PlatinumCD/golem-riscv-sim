#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PROJECT_ROOT}/third_party/riscv-gnu-toolchain"
readonly BUILD="${BUILD_ROOT}/riscv-gnu-toolchain"
readonly INSTALL="${INSTALL_ROOT}/riscv-gnu-toolchain"
readonly TARGET="riscv64-unknown-elf"

for command in git make; do
    require_command "${command}"
done
require_file "${SOURCE}/configure"

required_submodules=(binutils gcc newlib)
git -C "${SOURCE}" submodule sync -- "${required_submodules[@]}"
git -C "${SOURCE}" submodule update --init --jobs "${BUILD_JOBS}" \
    -- "${required_submodules[@]}"

mkdir -p -- "${BUILD}"
if [[ ! -f "${BUILD}/Makefile" ]]; then
    (
        cd -- "${BUILD}"
        "${SOURCE}/configure" \
            --prefix="${INSTALL}" \
            --with-arch=rv64gcv \
            --with-abi=lp64d \
            --disable-linux \
            --disable-gdb \
            --with-languages=c,c++
    )
fi

make -C "${BUILD}" -j "${BUILD_JOBS}"

require_executable "${INSTALL}/bin/${TARGET}-gcc"
require_executable "${INSTALL}/bin/${TARGET}-g++"
require_executable "${INSTALL}/bin/${TARGET}-ld"
require_file "${INSTALL}/${TARGET}/include/stdint.h"

if [[ "$("${INSTALL}/bin/${TARGET}-gcc" -dumpmachine)" != "${TARGET}" ]]; then
    echo "installed GNU compiler reports the wrong target" >&2
    exit 1
fi

echo "installed RISC-V GNU toolchain: ${INSTALL}"
