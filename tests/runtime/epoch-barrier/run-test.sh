#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/epoch-barrier"
readonly LOG="${OUTPUT_DIR}/simulation.log"

python3 "${TEST_DIR}/deployment_manifest_test.py"
"${PROJECT_ROOT}/build-scripts/build-platform.sh" epoch-barrier
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_EPOCH_BARRIER_TILE0="${OUTPUT_DIR}/tile0.elf"
export MITTENS_EPOCH_BARRIER_TILE1="${OUTPUT_DIR}/tile1.elf"
export MITTENS_EPOCH_BARRIER_STATS="${OUTPUT_DIR}/statistics.csv"
export MITTENS_EPOCH_BARRIER_DEPLOYMENT_MANIFEST="${TEST_DIR}/deployment-manifest.json"
export MITTENS_EPOCH_BARRIER_SERIAL="${OUTPUT_DIR}/serial"
mkdir -p -- "${MITTENS_EPOCH_BARRIER_SERIAL}"

rm -f -- "${LOG}" "${MITTENS_EPOCH_BARRIER_STATS}"
timeout 120s "${INSTALL_ROOT}/sst-core/bin/sst" \
    "${TEST_DIR}/simulation.py" 2>&1 | tee "${LOG}"

grep -F "epoch barrier tile 0: PASS" "${MITTENS_EPOCH_BARRIER_SERIAL}/tile-0.log" >/dev/null
grep -F "epoch barrier tile 1: PASS" "${MITTENS_EPOCH_BARRIER_SERIAL}/tile-1.log" >/dev/null
for epoch in 0 1 2; do
    grep -F "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=${epoch}" \
        "${LOG}" >/dev/null
done
if [[ "$(grep -Fc 'MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=' \
                 "${LOG}")" -ne 3 ]]; then
    echo "modeled epoch barrier released an unexpected number of epochs" >&2
    exit 1
fi
grep -F "completed_epoch=0 released_epoch=1 arrivals=2 idle=1" \
    "${LOG}" >/dev/null
grep -F "completed_epoch=1 released_epoch=2 arrivals=2 idle=1" \
    "${LOG}" >/dev/null
grep -F "completed_epoch=2 released_epoch=3 arrivals=2 idle=1" \
    "${LOG}" >/dev/null

parallel_pids=()
for instance in 0 1; do
    instance_log="${OUTPUT_DIR}/concurrent-${instance}.log"
    instance_stats="${OUTPUT_DIR}/concurrent-${instance}.csv"
    rm -f -- "${instance_log}" "${instance_stats}"
    (
        export MITTENS_EPOCH_BARRIER_STATS="${instance_stats}"
        export MITTENS_EPOCH_BARRIER_SERIAL="${OUTPUT_DIR}/concurrent-${instance}-serial"
        mkdir -p -- "${MITTENS_EPOCH_BARRIER_SERIAL}"
        timeout 120s "${INSTALL_ROOT}/sst-core/bin/sst" \
            "${TEST_DIR}/simulation.py" >"${instance_log}" 2>&1
    ) &
    parallel_pids+=("$!")
done
for pid in "${parallel_pids[@]}"; do
    wait "${pid}"
done
for instance in 0 1; do
    instance_log="${OUTPUT_DIR}/concurrent-${instance}.log"
    grep -F "epoch barrier tile 0: PASS" "${OUTPUT_DIR}/concurrent-${instance}-serial/tile-0.log" >/dev/null
    grep -F "epoch barrier tile 1: PASS" "${OUTPUT_DIR}/concurrent-${instance}-serial/tile-1.log" >/dev/null
    if [[ "$(grep -Fc 'MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=' \
                     "${instance_log}")" -ne 3 ]]; then
        echo "concurrent SST instance ${instance} had invalid releases" >&2
        exit 1
    fi
done

echo "QEMU/SST epoch barrier: PASS (2 guests, 3 epochs, explicit idle, cross-epoch RAM visibility, 2 concurrent isolated SST processes)"
