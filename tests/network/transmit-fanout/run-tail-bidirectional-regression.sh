#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/transmit-fanout"
readonly TRIAL_DIR="${OUTPUT_DIR}/tail-bidirectional-32"
readonly PROFILE_DIR="${TRIAL_DIR}/performance"
readonly TASK_DIR="${TRIAL_DIR}/tasks"
readonly LOG="${TRIAL_DIR}/simulation.log"
readonly STATS="${TRIAL_DIR}/router-statistics.csv"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly TILE_COUNT=32
readonly FANOUT=2
readonly PAYLOAD_WORDS=768
readonly PAYLOAD_BURSTS=1
readonly FRAME_WORDS=773
readonly SST_THREADS="${MITTENS_TAIL_SST_THREADS:-16}"
readonly SST_PARTITIONER="${MITTENS_TAIL_SST_PARTITIONER:-sst.simple}"

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

set +e
timeout --signal=TERM --kill-after=5s 240s \
    "${SST}" -n "${SST_THREADS}" \
    --partitioner="${SST_PARTITIONER}" \
    "${TEST_DIR}/tail-bidirectional.py" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e
if ((status != 0)); then
    echo "bidirectional fan-out regression did not complete: status=${status}" >&2
    exit "${status}"
fi

for ((tile = 0; tile < TILE_COUNT; ++tile)); do
    peer=$((TILE_COUNT - 1 - tile))
    network_trace="${PROFILE_DIR}/tile-${tile}-network.csv"
    dma_trace="${PROFILE_DIR}/tile-${tile}-receive-dma.csv"
    task_trace="${TASK_DIR}/tile-${tile}.csv"
    for file in "${network_trace}" "${dma_trace}" "${task_trace}"; do
        if [[ ! -s "${file}" ]]; then
            echo "missing bidirectional regression trace: ${file}" >&2
            exit 1
        fi
    done
    source_task=$((10000 + tile))
    if ! awk -F, -v task="${source_task}" \
        'NR > 1 && $2 == "finish" && $4 == task {found=1} END {exit !found}' \
        "${task_trace}"; then
        echo "source task ${source_task} on tile ${tile} did not finish" >&2
        exit 1
    fi
    for ((offset = 0; offset < FANOUT; ++offset)); do
        route_id=$((1000000 + peer * FANOUT + offset))
        task_id=$((100000 + peer * FANOUT + offset))
        arrived_words=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "arrive" && $6 == route {sum += $9} END {print sum + 0}' \
            "${network_trace}")
        dma_bursts=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "complete" && $4 == route {count++} END {print count + 0}' \
            "${dma_trace}")
        dma_words=$(awk -F, -v route="${route_id}" \
            'NR > 1 && $2 == "complete" && $4 == route {sum += $7} END {print sum + 0}' \
            "${dma_trace}")
        if ((arrived_words != FRAME_WORDS)); then
            echo "tile ${tile} route ${route_id}: expected ${FRAME_WORDS} arrived words, received ${arrived_words}" >&2
            exit 1
        fi
        if ((dma_bursts != PAYLOAD_BURSTS || dma_words != PAYLOAD_WORDS)); then
            echo "tile ${tile} route ${route_id}: expected ${PAYLOAD_BURSTS} DMA bursts/${PAYLOAD_WORDS} words, received ${dma_bursts}/${dma_words}" >&2
            exit 1
        fi
        if ! awk -F, -v task="${task_id}" \
            'NR > 1 && $2 == "finish" && $4 == task {found=1} END {exit !found}' \
            "${task_trace}"; then
            echo "destination task ${task_id} on tile ${tile} did not finish" >&2
            exit 1
        fi
    done
done

echo "bidirectional fan-out tail regression: PASS (32 tiles, 64 crossing routes, 768 words each, ${SST_THREADS} SST threads)"
