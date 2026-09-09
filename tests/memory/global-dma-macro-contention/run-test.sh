#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/global-dma-macro-contention"
readonly SCALAR_LOG="${OUTPUT_DIR}/scalar.log"
readonly MACRO_LOG="${OUTPUT_DIR}/macro.log"
readonly SCALAR_STATS="${OUTPUT_DIR}/scalar-statistics.csv"
readonly MACRO_STATS="${OUTPUT_DIR}/macro-statistics.csv"
readonly SCALAR_PROFILE="${OUTPUT_DIR}/scalar-profile"
readonly MACRO_PROFILE="${OUTPUT_DIR}/macro-profile"
readonly SCALAR_UART="${OUTPUT_DIR}/scalar-uart"
readonly MACRO_UART="${OUTPUT_DIR}/macro-uart"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" \
    global-dma-macro-contention
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_GLOBAL_DMA_MACRO_TILE0="${OUTPUT_DIR}/tile0.elf"
export MITTENS_GLOBAL_DMA_MACRO_TILE1="${OUTPUT_DIR}/tile1.elf"

rm -rf -- "${SCALAR_PROFILE}" "${MACRO_PROFILE}" \
    "${SCALAR_UART}" "${MACRO_UART}"
rm -f -- "${SCALAR_LOG}" "${MACRO_LOG}" \
    "${SCALAR_STATS}" "${MACRO_STATS}"
mkdir -p -- "${SCALAR_PROFILE}" "${MACRO_PROFILE}" \
    "${SCALAR_UART}" "${MACRO_UART}"

export MITTENS_GLOBAL_DMA_MACRO_STATS="${SCALAR_STATS}"
export MITTENS_GLOBAL_DMA_MACRO_PROFILE="${SCALAR_PROFILE}"
export MITTENS_GLOBAL_DMA_MACRO_UART="${SCALAR_UART}"
timeout --foreground --signal=TERM --kill-after=5s 60s \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${SCALAR_LOG}" 2>&1

export MITTENS_GLOBAL_DMA_MACRO_STATS="${MACRO_STATS}"
export MITTENS_GLOBAL_DMA_MACRO_PROFILE="${MACRO_PROFILE}"
export MITTENS_GLOBAL_DMA_MACRO_UART="${MACRO_UART}"
MITTENS_TEST_GLOBAL_DMA_MACRO_EXECUTION=1 \
timeout --foreground --signal=TERM --kill-after=5s 60s \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${MACRO_LOG}" 2>&1

for uart_dir in "${SCALAR_UART}" "${MACRO_UART}"; do
    for tile in 0 1; do
        grep -F "macro contention tile ${tile}: PASS" \
            "${uart_dir}/tile-${tile}.log" >/dev/null
    done
done
if [[ "$(grep -Fc 'dma_macro_records=2' "${MACRO_LOG}")" -ne 2 ]]; then
    echo "two-tile fixture did not execute one two-request macro per tile" >&2
    exit 1
fi

# The controller owns arbitration and exact readiness. Byte identity here is
# the primary proof that the macro preserved every physical request boundary
# and its arrival, release, service-start, and completion cycles.
cmp \
    "${SCALAR_PROFILE}/global-ram-requests.csv" \
    "${MACRO_PROFILE}/global-ram-requests.csv"
for tile in 0 1; do
    cmp \
        "${SCALAR_PROFILE}/tile-${tile}-summary.csv" \
        "${MACRO_PROFILE}/tile-${tile}-summary.csv"
    cut -d, --complement -f2 \
        "${SCALAR_PROFILE}/tile-${tile}-waits.csv" \
        > "${OUTPUT_DIR}/scalar-tile-${tile}-waits-normalized.csv"
    cut -d, --complement -f2 \
        "${MACRO_PROFILE}/tile-${tile}-waits.csv" \
        > "${OUTPUT_DIR}/macro-tile-${tile}-waits-normalized.csv"
    cmp \
        "${OUTPUT_DIR}/scalar-tile-${tile}-waits-normalized.csv" \
        "${OUTPUT_DIR}/macro-tile-${tile}-waits-normalized.csv"

    scalar_last_event="$(
        awk -F, 'NR > 1 && $2 + 0 > maximum { maximum = $2 + 0 }
                 END { print maximum }' \
            "${SCALAR_PROFILE}/tile-${tile}-waits.csv"
    )"
    macro_last_event="$(
        awk -F, 'NR > 1 && $2 + 0 > maximum { maximum = $2 + 0 }
                 END { print maximum }' \
            "${MACRO_PROFILE}/tile-${tile}-waits.csv"
    )"
    if [[ $((scalar_last_event - macro_last_event)) -ne 3 ]]; then
        echo "tile ${tile} macro did not remove exactly three envelopes" >&2
        exit 1
    fi
done

scalar_simulated_time="$(grep -F 'Simulation is complete' "${SCALAR_LOG}")"
macro_simulated_time="$(grep -F 'Simulation is complete' "${MACRO_LOG}")"
if [[ "${scalar_simulated_time}" != "${macro_simulated_time}" ]]; then
    echo "two-tile macro changed total simulated time" >&2
    exit 1
fi

python3 - \
    "${SCALAR_STATS}" \
    "${MACRO_STATS}" \
    "${SCALAR_PROFILE}/global-ram-requests.csv" <<'PY'
import csv
from pathlib import Path
import sys

expected = {
    "requests": 4,
    "bytes": 16384,
    "readiness_blocked_reads": 2,
    "readiness_releases": 2,
    "readiness_publications": 2,
    "readiness_duplicate_publications": 0,
    "readiness_maximum_waiters": 2,
    "readiness_execution_teardowns": 1,
}
for raw_path in sys.argv[1:3]:
    with Path(raw_path).open(encoding="utf-8", newline="") as source:
        actual = {
            row["StatisticName"]: int(row["Sum.u64"])
            for row in csv.DictReader(source)
            if row["ComponentName"] == "global_ram"
        }
    for name, value in expected.items():
        assert actual.get(name) == value, (raw_path, name, actual.get(name), value)

with Path(sys.argv[3]).open(encoding="utf-8", newline="") as source:
    requests = list(csv.DictReader(source))
assert len(requests) == 4, requests
assert {int(row["request_sequence"]) for row in requests} == {1, 2, 3, 4}
assert {int(row["tile_id"]) for row in requests} == {0, 1}
assert {row["direction"] for row in requests} == {"read", "write"}
assert all(int(row["byte_count"]) == 4096 for row in requests)
reads = [row for row in requests if row["direction"] == "read"]
assert len(reads) == 2
assert all(int(row["readiness_wait_cycles"]) > 0 for row in reads), reads
PY

echo "global DMA macro contention: PASS (two tiles, exact delayed readiness, byte-identical request and notification timelines, six envelopes removed)"
