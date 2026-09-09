#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/transmit-fanout"
readonly TRIAL_DIR="${OUTPUT_DIR}/software-payload"
readonly PROFILE_DIR="${TRIAL_DIR}/performance"
readonly TASK_DIR="${TRIAL_DIR}/tasks"
readonly LOG="${TRIAL_DIR}/simulation.log"
readonly STATS="${TRIAL_DIR}/router-statistics.csv"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${MITTENS_TEST_ELEMENT_LIBRARY:-${INSTALL_ROOT}/sst-elements/lib/sst-elements-library}"
readonly FRAME_COUNT=2
readonly PAYLOAD_WORDS=1022
readonly FRAME_WORDS=$((PAYLOAD_WORDS + 7))

MITTENS_FANOUT_BUILD_SOFTWARE_PAYLOAD_ONLY=1 \
    "${TEST_DIR}/build-test.sh"
mkdir -p -- "${PROFILE_DIR}" "${TASK_DIR}"
find "${PROFILE_DIR}" "${TASK_DIR}" -maxdepth 1 -type f -delete
rm -f -- "${LOG}" "${STATS}"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_FANOUT_ELF_DIRECTORY="${OUTPUT_DIR}"
export MITTENS_FANOUT_ELF_PREFIX="software-payload-2"
export MITTENS_TAIL_CONTENTION_SOURCES=1
export MITTENS_FANOUT_STATS="${STATS}"
export MITTENS_FANOUT_PROFILE="${PROFILE_DIR}"
export MITTENS_FANOUT_TASK_TRACE="${TASK_DIR}"

set +e
timeout --signal=TERM --kill-after=2s 12s \
    "${SST}" "${TEST_DIR}/tail-contention.py" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e
if ((status != 0)); then
    echo "software-payload regression did not complete: status=${status}" >&2
    exit "${status}"
fi

grep -F "SOFTWARE_PAYLOAD_SOURCE_PASS frames=${FRAME_COUNT}" \
    "${LOG}" >/dev/null
grep -F \
    "SOFTWARE_PAYLOAD_DESTINATION_PASS frames=${FRAME_COUNT} words=$((FRAME_COUNT * PAYLOAD_WORDS))" \
    "${LOG}" >/dev/null

readonly NETWORK_TRACE="${PROFILE_DIR}/tile-1-network.csv"
if [[ ! -s "${NETWORK_TRACE}" ]]; then
    echo "missing software-payload network trace: ${NETWORK_TRACE}" >&2
    exit 1
fi
arrived_words=$(awk -F, \
    'NR > 1 && $2 == "arrive" && $4 == 0 && $5 == 1 && \
     $6 >= 9000 && $6 < 9002 {sum += $10} END {print sum + 0}' \
    "${NETWORK_TRACE}")
if ((arrived_words != FRAME_COUNT * FRAME_WORDS)); then
    echo "expected $((FRAME_COUNT * FRAME_WORDS)) framed words, received ${arrived_words}" >&2
    exit 1
fi

readonly DMA_TRACE="${PROFILE_DIR}/tile-1-receive-dma.csv"
if [[ -s "${DMA_TRACE}" ]] && \
   awk -F, 'NR > 1 {found=1} END {exit !found}' "${DMA_TRACE}"; then
    echo "software-payload regression unexpectedly scheduled RX DMA" >&2
    exit 1
fi

echo "software-payload regression: PASS (${FRAME_COUNT} frames, $((FRAME_COUNT * PAYLOAD_WORDS)) software-consumed payload words, zero RX DMA)"
