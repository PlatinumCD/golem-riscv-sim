#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUT="${TEST_RESULTS_ROOT}/spm-chunking"
python3 "${TEST_DIR}/test_check.py"
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
for sizes in '65536 4096' '65548 4096' '131084 3072'; do
    read -r data_bytes chunk_bytes <<< "${sizes}"
    trial="${OUT}/d${data_bytes}-c${chunk_bytes}"
    mkdir -p "${trial}/profile/tasks"
    make -C "${TEST_DIR}" OUT="${trial}/guest" DATA_BYTES="${data_bytes}" \
        CHUNK_BYTES="${chunk_bytes}" > "${trial}/build.log" 2>&1
    MITTENS_TEST_ELF="${trial}/guest/tile.elf" MITTENS_TEST_PROFILE="${trial}/profile" \
        timeout --foreground 180 "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
        > "${trial}/simulation.log" 2>&1
    python3 "${TEST_DIR}/check.py" "${trial}" "${data_bytes}" "${chunk_bytes}" \
        "${GOLEM_LLVM_DIR}/bin/llvm-nm"
done
echo "SPM chunking: all cases PASS"
