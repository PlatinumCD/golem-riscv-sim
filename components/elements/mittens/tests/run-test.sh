#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"

if [[ ! -x "${SST}" || ! -x "${QEMU}" ]]; then
    echo "build QEMU, SST Core, and SST Elements before running Mittens tests" >&2
    exit 1
fi

"${PROJECT_ROOT}/build-scripts/build-platform.sh" hello
"${PROJECT_ROOT}/build-scripts/build-platform.sh" mesh-pair

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${BUILD_ROOT}/tests/hello/hello.elf"
export MITTENS_TILE0_ELF="${BUILD_ROOT}/tests/mesh-pair/tile0-sender.elf"
export MITTENS_TILE1_ELF="${BUILD_ROOT}/tests/mesh-pair/tile1-receiver.elf"

"${SST}" "${TEST_DIR}/component_lifecycle.py"
"${SST}" "${TEST_DIR}/qemu_boot.py"
MITTENS_TEST_TILES=4 "${SST}" "${TEST_DIR}/multi_tile_boot.py"
"${SST}" "${TEST_DIR}/two_tile_bridge.py"
