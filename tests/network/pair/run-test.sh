#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" mesh-pair

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TILE0_ELF="${TEST_RESULTS_ROOT}/mesh-pair/tile0-sender.elf"
export MITTENS_TILE1_ELF="${TEST_RESULTS_ROOT}/mesh-pair/tile1-receiver.elf"

exec "${SST}" "${TEST_DIR}/simulation.py"
