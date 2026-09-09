#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/platform/rvv-only-accounting"
readonly RESULTS="${OUTPUT_DIR}/results.csv"
readonly K_VALUES=(32 64 128 256 512 1024)

make -C "${TEST_DIR}"
rm -f -- "${RESULTS}"
printf 'K,vector_instructions,scalar_instructions,guest_cpu_cycles,'\
'measured_cycles,sync_events,total_vector_instructions,total_scalar_instructions\n' \
    > "${RESULTS}"

for k in "${K_VALUES[@]}"; do
    profile="${OUTPUT_DIR}/k${k}/profile"
    tasks="${OUTPUT_DIR}/k${k}/tasks"
    output="${OUTPUT_DIR}/k${k}.out"
    mkdir -p -- "${profile}" "${tasks}"
    rm -f -- "${profile}/tile-0-summary.csv" "${tasks}/tile-0.csv" "${output}"

    MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64" \
    MITTENS_TEST_ELF="${OUTPUT_DIR}/rvv-k${k}.elf" \
    MITTENS_TEST_PROFILE="${profile}" \
    MITTENS_TEST_TASKS="${tasks}" \
        "${INSTALL_ROOT}/sst-core/bin/sst" \
        "${TEST_DIR}/simulation.py" > "${output}"

    test -s "${profile}/tile-0-summary.csv"
    test -s "${tasks}/tile-0.csv"
    python3 - "${RESULTS}" "${k}" \
        "${profile}/tile-0-summary.csv" "${tasks}/tile-0.csv" <<'PY'
import csv
import sys

results, k_text, summary_path, task_path = sys.argv[1:]
k = int(k_text)
summary = {}
with open(summary_path, encoding="utf-8", newline="") as source:
    for row in csv.DictReader(source):
        summary[row["metric"]] = int(row["value"])

with open(task_path, encoding="utf-8", newline="") as source:
    tasks = list(csv.DictReader(source))
starts = [row for row in tasks if row["event"] == "start"]
finishes = [row for row in tasks if row["event"] == "finish"]
if len(starts) != 1 or len(finishes) != 1:
    raise SystemExit("expected exactly one task start and finish")
start, finish = starts[0], finishes[0]
measured_cycles = int(finish["cpu_cycles"]) - int(start["cpu_cycles"])
measured_instructions = (
    int(finish["retired_instructions"])
    - int(start["retired_instructions"])
)
if measured_instructions < k:
    raise SystemExit("measured instruction count is smaller than K")

finish_tick = summary["finish_tick"]
if finish_tick % 1000:
    raise SystemExit(f"finish tick is not an integer 1 GHz cycle: {finish_tick}")
if summary.get("scratchpad_service_cycles", 0) != 0:
    raise SystemExit("RVV-only diagnostic used scratchpad service")
if summary.get("physical_global_dma_submitted", 0) != 0:
    raise SystemExit("RVV-only diagnostic submitted global DMA")
if summary.get("network_packets", 0) != 0:
    raise SystemExit("RVV-only diagnostic generated NoC packets")

total_vector_instructions = summary["vector_instructions"]
total_scalar_instructions = summary["instructions"] - total_vector_instructions
timed_scalar_instructions = measured_instructions - k
with open(results, "a", encoding="utf-8", newline="") as output:
    output.write(
        f"{k},{k},{timed_scalar_instructions},{summary['cpu_cycles']},"
        f"{measured_cycles},{summary['synchronization_events']},"
        f"{total_vector_instructions},{total_scalar_instructions}\n"
    )
print(
    f"K={k}: timed_vector={k}, timed_scalar={timed_scalar_instructions}, "
    f"guest={summary['cpu_cycles']}, measured={measured_cycles}, "
    f"sync_events={summary['synchronization_events']}, "
    f"total_vector={total_vector_instructions}, "
    f"total_scalar={total_scalar_instructions}"
)
PY
done

printf 'results: %s\n' "${RESULTS}"
