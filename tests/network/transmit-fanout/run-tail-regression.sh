#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/transmit-fanout"
readonly TRIAL_DIR="${OUTPUT_DIR}/tail-regression"
readonly PROFILE_DIR="${TRIAL_DIR}/performance"
readonly TASK_DIR="${TRIAL_DIR}/tasks"
readonly LOG="${TRIAL_DIR}/simulation.log"
readonly STATS="${TRIAL_DIR}/router-statistics.csv"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly PAYLOAD_WORDS=768
readonly PAYLOAD_BURSTS=1
readonly FRAME_WORDS=773

"${TEST_DIR}/build-test.sh"
mkdir -p -- "${PROFILE_DIR}" "${TASK_DIR}"
find "${PROFILE_DIR}" "${TASK_DIR}" -maxdepth 1 -type f -delete
rm -f -- "${LOG}" "${STATS}"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_FANOUT_TILE0_ELF="${OUTPUT_DIR}/tail-regression-tile0.elf"
export MITTENS_FANOUT_TILE1_ELF="${OUTPUT_DIR}/tail-regression-tile1.elf"
export MITTENS_FANOUT_STATS="${STATS}"
export MITTENS_FANOUT_PROFILE="${PROFILE_DIR}"
export MITTENS_FANOUT_TASK_TRACE="${TASK_DIR}"

set +e
timeout --signal=TERM --kill-after=5s 120s \
    "${SST}" --heartbeat-wall-period=10s \
    "${TEST_DIR}/tail-regression.py" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e
if ((status != 0)); then
    echo "fan-out tail regression did not complete: status=${status}" >&2
    exit "${status}"
fi

grep -F "FANOUT_SOURCE_PASS fanout=2" "${LOG}" >/dev/null
grep -F "FANOUT_DESTINATION_PASS fanout=2" "${LOG}" >/dev/null

readonly NETWORK_TRACE="${PROFILE_DIR}/tile-1-network.csv"
readonly DMA_TRACE="${PROFILE_DIR}/tile-1-receive-dma.csv"
readonly TASK_TRACE="${TASK_DIR}/tile-1.csv"
for file in "${NETWORK_TRACE}" "${DMA_TRACE}" "${TASK_TRACE}"; do
    if [[ ! -s "${file}" ]]; then
        echo "missing regression trace: ${file}" >&2
        exit 1
    fi
done

for route_id in 1000 1001; do
    arrived_words=$(awk -F, -v route="${route_id}" \
        'NR > 1 && $2 == "arrive" && $6 == route {sum += $9} END {print sum + 0}' \
        "${NETWORK_TRACE}")
    dma_bursts=$(awk -F, -v route="${route_id}" \
        'NR > 1 && $2 == "complete" && $4 == route {count++} END {print count + 0}' \
        "${DMA_TRACE}")
    dma_words=$(awk -F, -v route="${route_id}" \
        'NR > 1 && $2 == "complete" && $4 == route {sum += $7} END {print sum + 0}' \
        "${DMA_TRACE}")
    if ((arrived_words != FRAME_WORDS)); then
        echo "route ${route_id}: expected ${FRAME_WORDS} arrived words, received ${arrived_words}" >&2
        exit 1
    fi
    if ((dma_bursts != PAYLOAD_BURSTS || dma_words != PAYLOAD_WORDS)); then
        echo "route ${route_id}: expected ${PAYLOAD_BURSTS} DMA bursts/${PAYLOAD_WORDS} words, received ${dma_bursts}/${dma_words}" >&2
        exit 1
    fi
done

for task_id in 100 101; do
    if ! awk -F, -v task="${task_id}" \
        'NR > 1 && $2 == "finish" && $4 == task {found=1} END {exit !found}' \
        "${TASK_TRACE}"; then
        echo "destination task ${task_id} did not finish" >&2
        exit 1
    fi
done

echo "fan-out tail regression: PASS (2 routes, 768 words each, memhierarchy)"
