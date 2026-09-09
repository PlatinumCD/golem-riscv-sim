#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SRC2_TEST_PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
export GOLEM_HARDWARE_TREE=src
source "${SRC2_TEST_PROJECT_ROOT}/build-scripts/common.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/mittens-memory-initialization-barrier"
readonly SERIAL_LOG="${OUTPUT_DIR}/serial.log"
readonly PARALLEL_LOG="${OUTPUT_DIR}/parallel.log"
readonly SERIAL_PROFILE="${OUTPUT_DIR}/serial-profile"
readonly PARALLEL_PROFILE="${OUTPUT_DIR}/parallel-profile"
readonly SERIAL_STATS="${OUTPUT_DIR}/serial-statistics.csv"
readonly PARALLEL_STATS="${OUTPUT_DIR}/parallel-statistics.csv"

if [[ ! -x "${SST}" || ! -d "${ELEMENT_LIBRARY}" ]]; then
    echo "build SST Core and the Mittens element before this test" >&2
    exit 1
fi

mkdir -p -- "${OUTPUT_DIR}" "${SERIAL_PROFILE}" "${PARALLEL_PROFILE}"
export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"

rm -f -- "${SERIAL_LOG}" "${PARALLEL_LOG}" \
    "${SERIAL_STATS}" "${PARALLEL_STATS}" \
    "${SERIAL_PROFILE}/memory-init-barrier.csv" \
    "${PARALLEL_PROFILE}/memory-init-barrier.csv"
MITTENS_MEMORY_INIT_BARRIER_PROFILE_DIR="${SERIAL_PROFILE}" \
MITTENS_MEMORY_INIT_BARRIER_STATS="${SERIAL_STATS}" \
    "${SST}" "${TEST_DIR}/memory_initialization_barrier.py" \
    >"${SERIAL_LOG}" 2>&1
MITTENS_MEMORY_INIT_BARRIER_PROFILE_DIR="${PARALLEL_PROFILE}" \
MITTENS_MEMORY_INIT_BARRIER_STATS="${PARALLEL_STATS}" \
    "${SST}" -n 2 --partitioner=sst.simple \
    "${TEST_DIR}/memory_initialization_barrier.py" \
    >"${PARALLEL_LOG}" 2>&1

for log in "${SERIAL_LOG}" "${PARALLEL_LOG}"; do
    if [[ "$(grep -c 'MITTENS_MEMORY_INIT_BARRIER_RELEASE' "${log}")" -ne 1 ||
          "$(grep -c 'MITTENS_MEMORY_INIT_BARRIER_PROBE' "${log}")" -ne 3 ]]; then
        echo "memory initialization barrier did not release exactly once: ${log}" >&2
        exit 1
    fi
    grep -F 'MITTENS_MEMORY_INIT_BARRIER_RELEASE arrivals=3' \
        "${log}" >/dev/null
    grep -F 'release_tiles_per_cycle=1' "${log}" >/dev/null
    mapfile -t release_cycles < <(
        sed -n 's/.*MITTENS_MEMORY_INIT_BARRIER_PROBE .*release_cycle=\([0-9][0-9]*\).*/\1/p' \
            "${log}" | sort -n
    )
    if [[ "${#release_cycles[@]}" -ne 3 ||
          "$((release_cycles[1] - release_cycles[0]))" -ne 1 ||
          "$((release_cycles[2] - release_cycles[1]))" -ne 1 ]]; then
        echo "memory initialization barrier did not honor finite release bandwidth: ${log}" >&2
        exit 1
    fi
    for tile in 0 2 3; do
        if [[ "$(grep -c "MITTENS_MEMORY_INIT_BARRIER_PROBE tile=${tile} " "${log}")" -ne 1 ]]; then
            echo "memory initialization tile ${tile} did not receive exactly one release: ${log}" >&2
            exit 1
        fi
    done
done

for evidence in \
    "${SERIAL_PROFILE}/memory-init-barrier.csv:${SERIAL_STATS}" \
    "${PARALLEL_PROFILE}/memory-init-barrier.csv:${PARALLEL_STATS}"; do
    IFS=: read -r timeline statistics <<<"${evidence}"
    python3 - "${timeline}" "${statistics}" <<'PY'
import csv
import sys

with open(sys.argv[1], encoding="utf-8", newline="") as source:
    timeline = list(csv.DictReader(source))
assert len(timeline) == 3, timeline
assert {int(row["tile_id"]) for row in timeline} == {0, 2, 3}, timeline
for row in timeline:
    arrival = int(row["arrival_cycle"])
    release = int(row["release_cycle"])
    wait = int(row["wait_cycles"])
    assert arrival <= release, row
    assert wait == release - arrival, row
assert [int(row["release_cycle"]) for row in timeline] == sorted(
    int(row["release_cycle"]) for row in timeline
), timeline
assert len({int(row["release_cycle"]) for row in timeline}) == 3, timeline

with open(sys.argv[2], encoding="utf-8", newline="") as source:
    statistics = {
        row["StatisticName"]: int(row["Sum.u64"])
        for row in csv.DictReader(source)
        if row["ComponentName"] == "memory_init_barrier"
    }
expected = {
    "arrivals": 3,
    "releases": 1,
    "barrier_wait_cycles": (
        max(int(row["release_cycle"]) for row in timeline) -
        min(int(row["arrival_cycle"]) for row in timeline)
    ),
    "tile_wait_cycles": sum(int(row["wait_cycles"]) for row in timeline),
}
for name, value in expected.items():
    assert statistics.get(name) == value, (name, statistics.get(name), value)
PY
done

grep -E 'MITTENS_MEMORY_INIT_BARRIER_PROBE .*sst_thread=0' \
    "${PARALLEL_LOG}" >/dev/null
grep -E 'MITTENS_MEMORY_INIT_BARRIER_PROBE .*sst_thread=1' \
    "${PARALLEL_LOG}" >/dev/null

readonly INVALID_LOG="${OUTPUT_DIR}/invalid-duplicate.log"
rm -f -- "${INVALID_LOG}"
if MITTENS_MEMORY_INIT_BARRIER_INVALID=duplicate \
    MITTENS_MEMORY_INIT_BARRIER_PROFILE_DIR="${OUTPUT_DIR}/invalid-profile" \
    MITTENS_MEMORY_INIT_BARRIER_STATS="${OUTPUT_DIR}/invalid-statistics.csv" \
    "${SST}" "${TEST_DIR}/memory_initialization_barrier.py" \
    >"${INVALID_LOG}" 2>&1; then
    echo "memory initialization barrier accepted a duplicate arrival" >&2
    exit 1
fi
grep -F 'memory initialization barrier duplicate arrival from tile 0' \
    "${INVALID_LOG}" >/dev/null

echo "event-driven memory initialization barrier: PASS (staggered serial, two-thread, exact arrival/release, duplicate rejection)"
