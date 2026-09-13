#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/global-ram-exact"
readonly LOG="${OUTPUT_DIR}/simulation.log"
readonly PROFILE_DIR="${OUTPUT_DIR}/profile"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" global-ram-exact
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_GLOBAL_RAM_EXACT_TILE0="${OUTPUT_DIR}/tile0.elf"
export MITTENS_GLOBAL_RAM_EXACT_TILE1="${OUTPUT_DIR}/tile1.elf"
export MITTENS_GLOBAL_RAM_EXACT_TILE2="${OUTPUT_DIR}/tile2.elf"
export MITTENS_GLOBAL_RAM_EXACT_STATS="${OUTPUT_DIR}/statistics.csv"
export MITTENS_GLOBAL_RAM_EXACT_PROFILE="${PROFILE_DIR}"

rm -f -- "${LOG}" "${MITTENS_GLOBAL_RAM_EXACT_STATS}"
mkdir -p -- "${PROFILE_DIR}" "${OUTPUT_DIR}/serial"
find "${PROFILE_DIR}" -maxdepth 1 -type f -delete
timeout --foreground --signal=TERM --kill-after=10s 300s \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" 2>&1 | \
    tee "${LOG}"

for tile in 0 1 2; do
    grep -F "exact global RAM tile ${tile}: PASS" "${OUTPUT_DIR}/serial/tile-${tile}.log" >/dev/null
done
if grep -F "memory_requests=" "${LOG}" | \
    grep -Ev "memory_requests=0([[:space:]]|$)" >/dev/null; then
    echo "exact global RAM unexpectedly used MemHierarchy" >&2
    exit 1
fi

python3 - "${MITTENS_GLOBAL_RAM_EXACT_STATS}" "${PROFILE_DIR}/global-ram-requests.csv" <<'PY'
import csv
import sys

with open(sys.argv[2], newline="") as stream:
    requests = list(csv.DictReader(stream))
boot = [r for r in requests if int(r["execution_id"]) == 0]
application = [r for r in requests if int(r["execution_id"]) == 91]
assert len(application) == 6 and sum(int(r["byte_count"]) for r in application) == 24576
assert boot and all(r["direction"] == "read" for r in boot)
expected = {
    "requests": 6 + len(boot),
    "bytes": 24576 + sum(int(r["byte_count"]) for r in boot),
    "readiness_blocked_reads": 4,
    "readiness_releases": 4,
    "readiness_interval_lookups": 8,
    "readiness_publications": 2,
    "readiness_duplicate_publications": 0,
    "readiness_maximum_waiters": 4,
    "readiness_execution_teardowns": 1,
}
with open(sys.argv[1], encoding="utf-8", newline="") as source:
    actual = {
        row["StatisticName"]: int(row["Sum.u64"])
        for row in csv.DictReader(source)
        if row["ComponentName"] == "global_ram"
    }
for name, value in expected.items():
    assert actual.get(name) == value, (name, actual.get(name), value)
PY

python3 - "${PROFILE_DIR}" <<'PY'
import csv
from pathlib import Path
import sys

profile = Path(sys.argv[1])
tile_rows = []
for path in sorted(profile.glob("tile-*-progress.csv")):
    with path.open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source))
    assert rows and rows[-1]["kind"] == "final", (path, rows)
    final = rows[-1]
    submitted = int(final["physical_global_dma_submitted"])
    completed = int(final["physical_global_dma_completed"])
    assert submitted == completed, (path, final)
    tile_rows.append((submitted, completed))
assert len(tile_rows) == 3, tile_rows
assert sum(row[0] for row in tile_rows) == 6, tile_rows

with (profile / "global-ram-progress.csv").open(
        encoding="utf-8", newline="") as source:
    controller = list(csv.DictReader(source))
assert controller and controller[-1]["kind"] == "final", controller
final = controller[-1]
with (profile / "global-ram-requests.csv").open() as stream:
    request_count = sum(1 for _ in csv.DictReader(stream))
assert int(final["physical_dma_submitted"]) == request_count, final
assert int(final["physical_dma_completed"]) == request_count, final
assert int(final["readiness_blocked"]) == 4, final
assert int(final["readiness_released"]) == 4, final
assert int(final["readiness_currently_blocked"]) == 0, final
assert int(final["queued_requests"]) == 0, final
assert int(final["active_requests"]) == 0, final
PY

echo "exact global RAM QEMU/SST: PASS (early reads, partial union, fanout, staged bytes, teardown)"
