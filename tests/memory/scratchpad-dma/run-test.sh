#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/output.txt"
readonly BURST_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/burst-output.txt"
readonly SCALAR_PROFILE_DIRECTORY="${TEST_RESULTS_ROOT}/scratchpad-dma/profile"
readonly SCALAR_STATISTICS="${TEST_RESULTS_ROOT}/scratchpad-dma/statistics.csv"
mkdir -p -- "${SCALAR_PROFILE_DIRECTORY}"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" scratchpad-dma
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_ELF="${TEST_RESULTS_ROOT}/scratchpad-dma/scratchpad-dma.elf"

MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY="${SCALAR_PROFILE_DIRECTORY}" \
MITTENS_TEST_STATISTICS="${SCALAR_STATISTICS}" \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${OUTPUT}"
grep -F "global DMA test: PASS" "${OUTPUT}"
python3 - \
    "${SCALAR_PROFILE_DIRECTORY}/tile-0-summary.csv" \
    "${SCALAR_STATISTICS}" <<'PY'
import csv
import sys

with open(sys.argv[1], encoding="utf-8", newline="") as source:
    summary = {
        row["metric"]: int(row["value"])
        for row in csv.DictReader(source)
    }
with open(sys.argv[2], encoding="utf-8", newline="") as source:
    statistics = {
        (row["ComponentName"], row["StatisticName"]): int(row["Sum.u64"])
        for row in csv.DictReader(source)
    }
service = statistics.get(("tile0", "scratchpad_service_cycles"))
assert service is not None and service > 0, service
assert summary.get("scratchpad_service_cycles") == service, (
    summary.get("scratchpad_service_cycles"), service
)
PY

export MITTENS_BURST_STATS="${TEST_RESULTS_ROOT}/scratchpad-dma/burst-stats.csv"
export MITTENS_BURST_SERIAL="${TEST_RESULTS_ROOT}/scratchpad-dma/serial"
mkdir -p -- "${MITTENS_BURST_SERIAL}"
"${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/burst-simulation.py" |
    tee "${BURST_OUTPUT}"
for tile in {0..16}; do
    grep -F "global DMA test: PASS" "${MITTENS_BURST_SERIAL}/tile-${tile}.log"
done
