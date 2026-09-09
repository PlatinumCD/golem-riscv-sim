#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly TEST_PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
export GOLEM_HARDWARE_TREE=src
source "${TEST_PROJECT_ROOT}/build-scripts/common.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"

mkdir -p -- "${BUILD_ROOT}/tests"
readonly OUTPUT_DIR="$(mktemp -d "${BUILD_ROOT}/tests/global-ram-readiness.XXXXXX")"
trap 'rm -rf -- "${OUTPUT_DIR}"' EXIT

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_GLOBAL_RAM_READINESS_STATS="${OUTPUT_DIR}/coverage.csv"
"${SST}" "${TEST_DIR}/global_ram_readiness.py" 2>&1 |
    tee "${OUTPUT_DIR}/coverage.log"
grep -F "tile 0 exact readiness: PASS" "${OUTPUT_DIR}/coverage.log" >/dev/null
grep -F "tile 1 exact readiness: PASS" "${OUTPUT_DIR}/coverage.log" >/dev/null

python3 - \
    "${MITTENS_GLOBAL_RAM_READINESS_STATS}" \
    "${OUTPUT_DIR}/global-ram-requests.csv" \
    "${OUTPUT_DIR}/global-ram-teardowns.csv" <<'PY'
import csv
import sys

expected = {
    "requests": 11,
    "bytes": 61440,
    "readiness_blocked_reads": 6,
    "readiness_releases": 6,
    "readiness_interval_lookups": 16,
    "readiness_publications": 4,
    "readiness_duplicate_publications": 0,
    "readiness_maximum_waiters": 4,
    "readiness_execution_teardowns": 2,
}
with open(sys.argv[1], encoding="utf-8", newline="") as source:
    actual = {
        row["StatisticName"]: int(row["Sum.u64"])
        for row in csv.DictReader(source)
        if row["ComponentName"] == "global_ram"
    }
for name, value in expected.items():
    assert actual.get(name) == value, (name, actual.get(name), value)

with open(sys.argv[2], encoding="utf-8", newline="") as source:
    requests = list(csv.DictReader(source))
assert len(requests) == expected["requests"], len(requests)
assert len({int(row["request_sequence"]) for row in requests}) == len(requests)
readiness_delay = 0
queue_delay = 0
service = 0
for row in requests:
    arrival = int(row["arrival_cycle"])
    readiness = int(row["readiness_cycle"])
    service_start = int(row["service_start_cycle"])
    completion = int(row["completion_cycle"])
    assert arrival <= readiness <= service_start <= completion, row
    assert int(row["readiness_wait_cycles"]) == readiness - arrival, row
    assert int(row["queue_cycles"]) == service_start - readiness, row
    assert int(row["service_cycles"]) == completion - service_start, row
    readiness_delay += int(row["readiness_wait_cycles"])
    queue_delay += int(row["queue_cycles"])
    service += int(row["service_cycles"])
assert actual.get("readiness_delay_cycles") == readiness_delay, (
    actual.get("readiness_delay_cycles"), readiness_delay
)
assert actual.get("queue_delay_cycles") == queue_delay, (
    actual.get("queue_delay_cycles"), queue_delay
)
assert actual.get("service_cycles") == service, (
    actual.get("service_cycles"), service
)

with open(sys.argv[3], encoding="utf-8", newline="") as source:
    teardowns = list(csv.DictReader(source))
assert len(teardowns) == 4, teardowns
by_execution = {}
teardown_wait = 0
for row in teardowns:
    execution = int(row["execution_id"])
    by_execution.setdefault(execution, set()).add(int(row["tile_id"]))
    arrival = int(row["arrival_cycle"])
    release = int(row["release_cycle"])
    wait = int(row["wait_cycles"])
    assert arrival <= release, row
    assert wait == release - arrival, row
    teardown_wait += wait
assert len(by_execution) == expected["readiness_execution_teardowns"], by_execution
assert all(tiles == {0, 1} for tiles in by_execution.values()), by_execution
assert actual.get("execution_teardown_wait_cycles") == teardown_wait, (
    actual.get("execution_teardown_wait_cycles"), teardown_wait
)
PY

python3 - "${OUTPUT_DIR}/global-ram-progress.csv" <<'PY'
import csv
import sys

with open(sys.argv[1], encoding="utf-8", newline="") as source:
    rows = list(csv.DictReader(source))
assert rows, "controller emitted no timeout-safe progress snapshots"
for row in rows:
    submitted = int(row["physical_dma_submitted"])
    completed = int(row["physical_dma_completed"])
    queued = int(row["queued_requests"])
    active = int(row["active_requests"])
    blocked = int(row["readiness_blocked"])
    released = int(row["readiness_released"])
    current = int(row["readiness_currently_blocked"])
    assert submitted == completed + queued + active, row
    assert blocked == released + current, row
kinds = {row["kind"] for row in rows}
assert "readiness-blocked" in kinds, kinds
assert "readiness-released" in kinds, kinds
final = rows[-1]
assert final["kind"] == "final", final
expected = {
    "physical_dma_submitted": 11,
    "physical_dma_completed": 11,
    "physical_dma_bytes_completed": 61440,
    "readiness_blocked": 6,
    "readiness_released": 6,
    "readiness_currently_blocked": 0,
    "readiness_publications": 4,
    "queued_requests": 0,
    "active_requests": 0,
}
for name, value in expected.items():
    assert int(final[name]) == value, (name, final[name], value)
PY

export MITTENS_GLOBAL_RAM_READINESS_SCENARIO=demand
export MITTENS_GLOBAL_RAM_READINESS_STATS="${OUTPUT_DIR}/demand.csv"
"${SST}" "${TEST_DIR}/global_ram_readiness.py" 2>&1 |
    tee "${OUTPUT_DIR}/demand.log"
grep -F "tile 0 blocked-read demand priority: PASS" \
    "${OUTPUT_DIR}/demand.log" >/dev/null

export MITTENS_GLOBAL_RAM_READINESS_SCENARIO=priority
export MITTENS_GLOBAL_RAM_READINESS_STATS="${OUTPUT_DIR}/priority.csv"
"${SST}" "${TEST_DIR}/global_ram_readiness.py" 2>&1 |
    tee "${OUTPUT_DIR}/priority.log"
grep -F "tile 0 bounded read priority: PASS" \
    "${OUTPUT_DIR}/priority.log" >/dev/null

export MITTENS_GLOBAL_RAM_READINESS_SCENARIO=reservation
export MITTENS_GLOBAL_RAM_READINESS_STATS="${OUTPUT_DIR}/reservation.csv"
"${SST}" "${TEST_DIR}/global_ram_readiness.py" 2>&1 |
    tee "${OUTPUT_DIR}/reservation.log"
grep -F "tile 0 reserved read channel: PASS" \
    "${OUTPUT_DIR}/reservation.log" >/dev/null

export MITTENS_GLOBAL_RAM_READINESS_SCENARIO=duplicate
export MITTENS_GLOBAL_RAM_READINESS_STATS="${OUTPUT_DIR}/duplicate.csv"
set +e
"${SST}" "${TEST_DIR}/global_ram_readiness.py" \
    >"${OUTPUT_DIR}/duplicate.log" 2>&1
readonly duplicate_status=$?
set -e
if [[ "${duplicate_status}" -eq 0 ]]; then
    echo "duplicate exact publication unexpectedly succeeded" >&2
    exit 1
fi
grep -F "duplicate or overlapping exact global RAM publication" \
    "${OUTPUT_DIR}/duplicate.log" >/dev/null

echo "global RAM exact readiness controller: PASS"
