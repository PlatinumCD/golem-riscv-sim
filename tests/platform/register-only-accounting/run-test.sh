#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/platform/register-only-accounting"
readonly RESULTS="${OUTPUT_DIR}/results.csv"

make -C "${TEST_DIR}"
rm -f -- "${RESULTS}"
printf 'mode,total_cycles,guest_cpu_cycles,measured_cycles,'\
'measured_instructions,scalar_instructions,vector_instructions,'\
'synchronization_events,synchronization_grants,non_cpu_cycles,'\
'scratchpad_service_cycles,guest_cycles_per_instruction,non_guest_fraction\n' \
    > "${RESULTS}"

for mode in scalar rvv; do
    profile="${OUTPUT_DIR}/${mode}/profile"
    tasks="${OUTPUT_DIR}/${mode}/tasks"
    output="${OUTPUT_DIR}/${mode}.out"
    mkdir -p -- "${profile}" "${tasks}"
    rm -f -- "${profile}/tile-0-summary.csv" "${tasks}/tile-0.csv" "${output}"

    MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64" \
    MITTENS_TEST_ELF="${OUTPUT_DIR}/${mode}.elf" \
    MITTENS_TEST_PROFILE="${profile}" \
    MITTENS_TEST_TASKS="${tasks}" \
        "${INSTALL_ROOT}/sst-core/bin/sst" \
        "${TEST_DIR}/simulation.py" > "${output}"

    test -s "${profile}/tile-0-summary.csv"
    test -s "${tasks}/tile-0.csv"
    PYTHONPATH="${PROJECT_ROOT}/tests/support${PYTHONPATH:+:${PYTHONPATH}}" python3 - "${RESULTS}" "${mode}" \
        "${profile}/tile-0-summary.csv" "${tasks}/tile-0.csv" <<'PY'
import csv
import sys
from pathlib import Path
from region import assert_register_only

results, mode, summary_path, task_path = sys.argv[1:]
summary = {}
with open(summary_path, encoding="utf-8", newline="") as source:
    for row in csv.DictReader(source):
        summary[row["metric"]] = int(row["value"])

with open(task_path, encoding="utf-8", newline="") as source:
    tasks = list(csv.DictReader(source))
starts = [row for row in tasks if row["event"] == "start"]
finishes = [row for row in tasks if row["event"] == "finish"]
if len(starts) != 1 or len(finishes) != 1:
    raise SystemExit(
        f"expected one start and finish, got {len(starts)} / {len(finishes)}"
    )
start, finish = starts[0], finishes[0]
measured_cycles = int(finish["cpu_cycles"]) - int(start["cpu_cycles"])
measured_instructions = (
    int(finish["retired_instructions"])
    - int(start["retired_instructions"])
)
if measured_cycles <= 0 or measured_instructions <= 0:
    raise SystemExit("measured task has no retired work")

finish_tick = summary["finish_tick"]
if finish_tick % 1000:
    raise SystemExit(f"finish tick is not an integer 1 GHz cycle: {finish_tick}")
total_cycles = finish_tick // 1000
guest_cycles = summary["cpu_cycles"]
non_cpu_cycles = total_cycles - guest_cycles
non_guest_fraction = non_cpu_cycles / total_cycles
assert_register_only(Path(summary_path).parent, start, finish)
spm_cycles = 0  # Proven CPU-data service in the measured region, not whole-run fills.

with open(results, "a", encoding="utf-8", newline="") as output:
    output.write(
        f"{mode},{total_cycles},{guest_cycles},{measured_cycles},"
        f"{measured_instructions},"
        f"{summary['instructions'] - summary['vector_instructions']},"
        f"{summary['vector_instructions']},{summary['synchronization_events']},"
        f"{summary['synchronization_grants']},{non_cpu_cycles},"
        f"{spm_cycles},{guest_cycles / summary['instructions']:.6f},"
        f"{non_guest_fraction:.6f}\n"
    )
print(
    f"{mode}: total={total_cycles}, guest={guest_cycles}, "
    f"measured={measured_cycles} cycles/{measured_instructions} instructions, "
    f"sync_events={summary['synchronization_events']}, "
    f"sync_grants={summary['synchronization_grants']}, SPM={spm_cycles}"
)
PY
done

printf 'results: %s\n' "${RESULTS}"
