#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/transmit-fanout"
readonly SOURCE_COUNT="${MITTENS_TAIL_CONTENTION_SOURCES:-8}"
if [[ ! "${SOURCE_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_TAIL_CONTENTION_SOURCES must be positive" >&2
    exit 2
fi
readonly TRIAL_DIR="${OUTPUT_DIR}/tail-contention-${SOURCE_COUNT}"
readonly PROFILE_DIR="${TRIAL_DIR}/performance"
readonly TASK_DIR="${TRIAL_DIR}/tasks"
readonly LOG="${TRIAL_DIR}/simulation.log"
readonly STATS="${TRIAL_DIR}/router-statistics.csv"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly FANOUT=2
readonly PAYLOAD_WORDS=768
readonly PAYLOAD_BURSTS=1
readonly FRAME_WORDS=775

"${TEST_DIR}/build-test.sh"
mkdir -p -- "${PROFILE_DIR}" "${TASK_DIR}"
find "${PROFILE_DIR}" "${TASK_DIR}" -maxdepth 1 -type f -delete
rm -f -- "${LOG}" "${STATS}"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_FANOUT_ELF_DIRECTORY="${OUTPUT_DIR}"
export MITTENS_FANOUT_STATS="${STATS}"
export MITTENS_FANOUT_PROFILE="${PROFILE_DIR}"
export MITTENS_FANOUT_TASK_TRACE="${TASK_DIR}"
export MITTENS_TAIL_CONTENTION_SOURCES="${SOURCE_COUNT}"

set +e
timeout --signal=TERM --kill-after=5s 180s \
    "${SST}" --heartbeat-wall-period=10s \
    "${TEST_DIR}/tail-contention.py" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e
if ((status != 0)); then
    echo "contended fan-out regression did not complete: status=${status}" >&2
    exit "${status}"
fi

readonly NETWORK_TRACE="${PROFILE_DIR}/tile-${SOURCE_COUNT}-network.csv"
readonly DMA_TRACE="${PROFILE_DIR}/tile-${SOURCE_COUNT}-receive-dma.csv"
readonly TASK_TRACE="${TASK_DIR}/tile-${SOURCE_COUNT}.csv"
for file in "${NETWORK_TRACE}" "${DMA_TRACE}" "${TASK_TRACE}"; do
    if [[ ! -s "${file}" ]]; then
        echo "missing contended regression trace: ${file}" >&2
        exit 1
    fi
done

for ((source = 0; source < SOURCE_COUNT; ++source)); do
    source_task_id=$((11 + source * FANOUT))
    source_trace="${TASK_DIR}/tile-${source}.csv"
    if [[ ! -s "${source_trace}" ]] ||
       ! awk -F, -v task="${source_task_id}" \
           'NR > 1 && $2 == "finish" && $4 == task {found=1} END {exit !found}' \
           "${source_trace}"; then
        echo "source task ${source_task_id} on tile ${source} did not finish" >&2
        exit 1
    fi
    for ((route_offset = 0; route_offset < FANOUT; ++route_offset)); do
        route_id=$((1000 + source * FANOUT + route_offset))
        task_id=$((100 + source * FANOUT + route_offset))
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
        if ((dma_bursts != PAYLOAD_BURSTS || dma_words != PAYLOAD_WORDS)); then
            echo "route ${route_id}: expected ${PAYLOAD_BURSTS} DMA bursts/${PAYLOAD_WORDS} words, received ${dma_bursts}/${dma_words}" >&2
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

echo "contended fan-out tail regression: PASS (${SOURCE_COUNT} sources, $((SOURCE_COUNT * FANOUT)) routes, 768 words each)"
