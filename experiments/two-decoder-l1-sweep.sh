#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../build-scripts/common.sh
source "${SCRIPT_DIR}/../build-scripts/common.sh"

readonly OUTPUT_ROOT="${GOLEM_L1_SWEEP_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/two-decoder-l1-sweep}"
readonly SHARED_BUILD="${OUTPUT_ROOT}/shared-build"
readonly TRIAL_ROOT="${OUTPUT_ROOT}/trials"
readonly SUMMARY="${OUTPUT_ROOT}/sweep-results.csv"
readonly ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly SYMBOLIZER="${INSTALL_ROOT}/llvm/bin/llvm-symbolizer"

readonly -a TRIALS=(
    "l1-1:memhierarchy:1"
    "l1-2:memhierarchy:2"
    "l1-4:memhierarchy:4"
    "l1-8:memhierarchy:8"
    "l1-16:memhierarchy:16"
    "native-bound:native:2"
)

clear_directory() {
    local directory="$1"
    if [[ -d "${directory}" ]]; then
        find "${directory}" -mindepth 1 -depth -delete
    fi
}

prepare_shared_build() {
    if [[ -s "${SHARED_BUILD}/active-cores.txt" &&
          -d "${SHARED_BUILD}/compiler/cores" ]]; then
        echo "reusing shared two-decoder compiler output"
        return
    fi

    echo "building the shared two-decoder deployment"
    GOLEM_EXPERIMENT_OUTPUT_DIR="${SHARED_BUILD}" \
        "${SCRIPT_DIR}/two-decoder-test.sh" --remove-intermediate-mlir
}

snapshot_trial() {
    local trial_directory="$1"
    mkdir -p -- "${trial_directory}"
    cp -a \
        "${SHARED_BUILD}/result.csv" \
        "${SHARED_BUILD}/router-statistics.csv" \
        "${SHARED_BUILD}/simulation.log" \
        "${SHARED_BUILD}/parameters.env" \
        "${SHARED_BUILD}/active-cores.txt" \
        "${trial_directory}/"
    cp -a "${SHARED_BUILD}/trace" "${trial_directory}/trace"
}

analyze_trial() {
    local trial_directory="$1"
    "${ANALYZER}" \
        "${trial_directory}/trace/performance" \
        "${trial_directory}/analysis" \
        --router-statistics "${trial_directory}/router-statistics.csv" \
        --task-trace-directory "${trial_directory}/trace/tasks" \
        --elf-directory "${SHARED_BUILD}/elf" \
        --symbolizer "${SYMBOLIZER}" \
        --mesh-link-width-bits 32 \
        --mesh-link-clock 1GHz
}

run_trial() {
    local specification="$1"
    local name backend latency
    IFS=: read -r name backend latency <<<"${specification}"
    local trial_directory="${TRIAL_ROOT}/${name}"

    if [[ -f "${trial_directory}/.complete" ]]; then
        echo "${name}: already complete"
        return
    fi

    clear_directory "${trial_directory}"
    clear_directory "${SHARED_BUILD}/trace"

    echo "${name}: backend=${backend}, L1 latency=${latency} cycle(s)"
    GOLEM_EXPERIMENT_OUTPUT_DIR="${SHARED_BUILD}" \
    GOLEM_MODEL_MEMORY_BACKEND="${backend}" \
    GOLEM_MODEL_L1_ACCESS_LATENCY_CYCLES="${latency}" \
    SCULPTOR_PLACEMENT_SCHEDULE=snake \
    SCULPTOR_NETWORK_WORD_BITS=32 \
    GOLEM_MODEL_NETWORK_BUFFER_CELLS=16 \
    GOLEM_MODEL_NETWORK_PACKET_WORDS=16 \
        "${SCRIPT_DIR}/two-decoder-test.sh" \
            --run --trace --resume-build

    snapshot_trial "${trial_directory}"
    analyze_trial "${trial_directory}"
    date -u +'%Y-%m-%dT%H:%M:%SZ' >"${trial_directory}/.complete"
}

write_summary() {
    printf '%s\n' \
        'trial,backend,l1_latency_cycles,simulated_time,wall_seconds,memory_requests,memory_latency_ticks,task_186_us,task_187_us,task_412_us,task_413_us' \
        >"${SUMMARY}"

    local specification name backend latency trial_directory
    for specification in "${TRIALS[@]}"; do
        IFS=: read -r name backend latency <<<"${specification}"
        trial_directory="${TRIAL_ROOT}/${name}"
        if [[ ! -f "${trial_directory}/.complete" ]]; then
            continue
        fi
        "${COMPILER_PYTHON}" - \
            "${name}" "${backend}" "${latency}" \
            "${trial_directory}/result.csv" \
            "${trial_directory}/analysis/summary.json" \
            "${trial_directory}/analysis/tasks.csv" <<'PY' >>"${SUMMARY}"
import csv
import json
import sys

name, backend, latency, result_path, summary_path, tasks_path = sys.argv[1:]
with open(result_path, encoding="utf-8", newline="") as source:
    result = next(csv.DictReader(source))
with open(summary_path, encoding="utf-8") as source:
    summary = json.load(source)
durations = {task_id: "" for task_id in ("186", "187", "412", "413")}
with open(tasks_path, encoding="utf-8", newline="") as source:
    for row in csv.DictReader(source):
        if row["task_id"] in durations:
            durations[row["task_id"]] = f'{int(row["duration_ticks"]) / 1e6:.3f}'
print(",".join([
    name,
    backend,
    latency,
    result["simulated_time"],
    result["wall_seconds"],
    str(summary["memory"]["modeled_requests"]),
    str(summary["memory"]["total_latency_ticks"]),
    *(durations[task_id] for task_id in ("186", "187", "412", "413")),
]))
PY
    done
}

mkdir -p -- "${OUTPUT_ROOT}" "${TRIAL_ROOT}"
prepare_shared_build

index=0
for trial in "${TRIALS[@]}"; do
    ((index += 1))
    echo "[${index}/${#TRIALS[@]}]"
    run_trial "${trial}"
done

write_summary
echo "L1 sweep complete: ${SUMMARY}"
