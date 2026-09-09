#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_ROOT="${TEST_RESULTS_ROOT}/analog-timing-validation"
readonly ANALYZER="${TEST_DIR}/analyze-results.py"
readonly RESULTS="${OUTPUT_ROOT}/results.csv"

require_executable "${SST}"
require_executable "${QEMU}"
require_executable "${ANALYZER}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" analog-timing-validation

run_case() {
    local name="$1"
    local arrays="$2"
    local elf="${OUTPUT_ROOT}/analog-timing-${name}.elf"
    local trial="${OUTPUT_ROOT}/${name}"
    local profile="${trial}/profile"
    local log="${trial}/simulation.log"

    require_file "${elf}"
    mkdir -p -- "${profile}"
    rm -f -- "${profile}/tile-0-analog.csv" \
        "${profile}/tile-0-summary.csv" \
        "${profile}/tile-0-progress.csv" \
        "${log}"

    echo "[analog timing] case=${name} arrays=${arrays}"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_TEST_ELF="${elf}" \
    MITTENS_ANALOG_TIMING_ARRAY_COUNT="${arrays}" \
    MITTENS_ANALOG_TIMING_PROFILE="${profile}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${log}"

    if ! grep -q "ANALOG_TIMING_${name^^}_PASS" "${log}"; then
        echo "${name} guest did not report a valid numerical result" >&2
        return 1
    fi
    require_file "${profile}/tile-0-analog.csv"
    require_file "${profile}/tile-0-summary.csv"
    require_file "${profile}/tile-0-progress.csv"
    python3 - "${profile}" <<'PY'
import csv
from pathlib import Path
import sys

profile = Path(sys.argv[1])
with (profile / "tile-0-progress.csv").open(
        encoding="utf-8", newline="") as source:
    progress = list(csv.DictReader(source))
assert progress and progress[-1]["kind"] == "final", progress
final = progress[-1]
submitted = int(final["analog_commands_submitted"])
completed = int(final["analog_commands_completed"])
assert submitted == completed and submitted > 0, final

with (profile / "tile-0-summary.csv").open(
        encoding="utf-8", newline="") as source:
    summary = {row["metric"]: int(row["value"])
               for row in csv.DictReader(source)}
assert summary["analog_commands_submitted"] == submitted, summary
assert summary["analog_commands_completed"] == completed, summary
PY
}

run_case single 1
run_case dual 2

"${ANALYZER}" \
    "${RESULTS}" \
    "single:1:${OUTPUT_ROOT}/single/profile/tile-0-analog.csv:${OUTPUT_ROOT}/single/profile/tile-0-summary.csv" \
    "dual:2:${OUTPUT_ROOT}/dual/profile/tile-0-analog.csv:${OUTPUT_ROOT}/dual/profile/tile-0-summary.csv"

echo "end-to-end QEMU/SST analog timing validation: PASS"
