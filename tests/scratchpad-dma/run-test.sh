#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly OUTPUT="${BUILD_ROOT}/tests/scratchpad-dma/output.txt"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" scratchpad-dma
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_ELF="${BUILD_ROOT}/tests/scratchpad-dma/scratchpad-dma.elf"

"${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${OUTPUT}"
grep -F "scratchpad DMA test: PASS" "${OUTPUT}"
