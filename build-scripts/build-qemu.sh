#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PREPARED_SOURCE_ROOT}/qemu"
readonly BUILD="${BUILD_ROOT}/qemu"
readonly INSTALL="${INSTALL_ROOT}/qemu"

if [[ -n "${QEMU_PYTHON:-}" ]]; then
    readonly PYTHON="${QEMU_PYTHON}"
elif [[ -x /usr/bin/python3 ]]; then
    readonly PYTHON=/usr/bin/python3
else
    readonly PYTHON="$(command -v python3)"
fi

for command in ninja python3; do
    require_command "${command}"
done
"${SCRIPT_DIR}/prepare-qemu.sh"

mkdir -p -- "${BUILD}" "${INSTALL}"
if [[ ! -f "${BUILD}/build.ninja" ]]; then
    (
        cd -- "${BUILD}"
        "${SOURCE}/configure" \
            --python="${PYTHON}" \
            --prefix="${INSTALL}" \
            --target-list=riscv64-softmmu \
            --disable-docs \
            --disable-guest-agent \
            --disable-plugins \
            --disable-slirp \
            --disable-tools \
            --disable-werror
    )
fi

ninja -C "${BUILD}" -j "${BUILD_JOBS}" qemu-system-riscv64
install -D -m 0755 \
    "${BUILD}/qemu-system-riscv64" \
    "${INSTALL}/bin/qemu-system-riscv64"

require_executable "${INSTALL}/bin/qemu-system-riscv64"
echo "installed Mittens QEMU: ${INSTALL}"
