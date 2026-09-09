#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/transmit-fanout"
readonly TRIAL_DIR="${OUTPUT_DIR}/receive-head-blocking"
readonly PROFILE_DIR="${TRIAL_DIR}/performance"
readonly TASK_DIR="${TRIAL_DIR}/tasks"
readonly LOG="${TRIAL_DIR}/simulation.log"
readonly STATS="${TRIAL_DIR}/router-statistics.csv"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${MITTENS_TEST_ELEMENT_LIBRARY:-${INSTALL_ROOT}/sst-elements/lib/sst-elements-library}"
readonly FANOUT=8
readonly SOURCE_COUNT=2
readonly DESTINATION_TILE=2
readonly PAYLOAD_WORDS=16
readonly FRAME_WORDS=23
readonly BRIDGE_BURST_CAPACITY=4

"${TEST_DIR}/build-test.sh"
mkdir -p -- "${PROFILE_DIR}" "${TASK_DIR}"
find "${PROFILE_DIR}" "${TASK_DIR}" -maxdepth 1 -type f -delete
rm -f -- "${LOG}" "${STATS}"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_FANOUT_ELF_DIRECTORY="${OUTPUT_DIR}"
export MITTENS_FANOUT_ELF_PREFIX="receive-head-blocking-2"
export MITTENS_TAIL_CONTENTION_SOURCES="${SOURCE_COUNT}"
export MITTENS_FANOUT_STATS="${STATS}"
export MITTENS_FANOUT_PROFILE="${PROFILE_DIR}"
export MITTENS_FANOUT_TASK_TRACE="${TASK_DIR}"
export MITTENS_FANOUT_RX_DMA_SETUP_CYCLES=100000

set +e
timeout --signal=TERM --kill-after=5s 30s \
    "${SST}" "${TEST_DIR}/tail-contention.py" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e
if ((status != 0)); then
    echo "receive-head-blocking regression did not complete: status=${status}" >&2
    exit "${status}"
fi

grep -F "FANOUT_DESTINATION_PASS fanout=${FANOUT}" "${LOG}" >/dev/null

readonly NETWORK_TRACE="${PROFILE_DIR}/tile-${DESTINATION_TILE}-network.csv"
readonly DMA_TRACE="${PROFILE_DIR}/tile-${DESTINATION_TILE}-receive-dma.csv"
readonly TASK_TRACE="${TASK_DIR}/tile-${DESTINATION_TILE}.csv"
for file in "${NETWORK_TRACE}" "${DMA_TRACE}" "${TASK_TRACE}"; do
    if [[ ! -s "${file}" ]]; then
        echo "missing receive-head-blocking regression trace: ${file}" >&2
        exit 1
    fi
done

first_dma_tick=$(awk -F, \
    'NR > 1 && $2 == "schedule" {print $9; exit}' \
    "${DMA_TRACE}")
if [[ -z "${first_dma_tick}" ]]; then
    echo "receive-head-blocking regression did not schedule an RX DMA" >&2
    exit 1
fi
headers_before_dma=$(awk -F, -v tick="${first_dma_tick}" \
    'NR > 1 && $2 == "arrive" && $4 == 0 && $5 == 2 && \
     $9 == "frame-header" && $17 < tick {count++} END {print count + 0}' \
    "${NETWORK_TRACE}")
if ((headers_before_dma < BRIDGE_BURST_CAPACITY)); then
    echo "receive delay did not fill all bridge slots before the first DMA" >&2
    exit 1
fi

for ((source = 0; source < SOURCE_COUNT; ++source)); do
    for ((offset = 0; offset < FANOUT; ++offset)); do
        route_id=$((1000 + source * FANOUT + offset))
        task_id=$((100 + source * FANOUT + offset))
        arrived_words=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "arrive" && $6 == route {sum += $10} END {print sum + 0}' \
            "${NETWORK_TRACE}")
        dma_bursts=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "complete" && $4 == route {count++} END {print count + 0}' \
            "${DMA_TRACE}")
        dma_words=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "complete" && $4 == route {sum += $8} END {print sum + 0}' \
            "${DMA_TRACE}")
        if ((arrived_words != FRAME_WORDS)); then
            echo "route ${route_id}: expected ${FRAME_WORDS} arrived words, received ${arrived_words}" >&2
            exit 1
        fi
        if ((dma_bursts != 1 || dma_words != PAYLOAD_WORDS)); then
            echo "route ${route_id}: expected 1 DMA burst/${PAYLOAD_WORDS} words, received ${dma_bursts}/${dma_words}" >&2
            exit 1
        fi
        if ! awk -F, -v task="${task_id}" \
            'NR > 1 && $2 == "finish" && $4 == task {found=1} END {exit !found}' \
            "${TASK_TRACE}"; then
            echo "destination task ${task_id} did not finish" >&2
            exit 1
        fi
    done
done

echo "receive-head-blocking regression: PASS ($((SOURCE_COUNT * FANOUT)) ordered cross-source frames, full-header bridge drained)"
