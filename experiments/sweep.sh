#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=params.sh
source "${SCRIPT_DIR}/params.sh"

usage() {
    echo "usage: $0 [--run]" >&2
}

RUN_EXPERIMENT=false
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --run)
            RUN_EXPERIMENT=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done
readonly RUN_EXPERIMENT

readonly SWEEP_ROOT="${GOLEM_SWEEP_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/parallel-workers}"
readonly TOTAL_RUNS="${#EXPERIMENT_PARALLEL_WORKER_VALUES[@]}"
runner_options=()
if [[ "${RUN_EXPERIMENT}" == true ]]; then
    runner_options+=(--run)
fi

mkdir -p -- "${SWEEP_ROOT}"

failures=0
completed=0
skipped=0
index=0

for parallel_workers in "${EXPERIMENT_PARALLEL_WORKER_VALUES[@]}"; do
    ((index += 1))
    run_name="workers-${parallel_workers}"
    run_dir="${SWEEP_ROOT}/${run_name}"
    completion_marker="${run_dir}/.complete"

    completion_is_valid=false
    if [[ -f "${completion_marker}" ]]; then
        if [[ "${RUN_EXPERIMENT}" == false || -f "${run_dir}/result.csv" ]]; then
            completion_is_valid=true
        fi
    fi
    if [[ "${completion_is_valid}" == true && "${GOLEM_SWEEP_FORCE:-false}" != true ]]; then
        echo "[${index}/${TOTAL_RUNS}] ${run_name}: already complete"
        ((skipped += 1))
        continue
    fi

    mkdir -p -- "${run_dir}"
    echo "[${index}/${TOTAL_RUNS}] ${run_name}: running"
    require_change=true
    if [[ "${parallel_workers}" -eq 1 ]]; then
        require_change=false
    fi
    if GOLEM_EXPERIMENT_OUTPUT_DIR="${run_dir}" \
       SCULPTOR_PARALLEL_WORKERS="${parallel_workers}" \
       SCULPTOR_REQUIRE_CHANGE="${require_change}" \
       "${SCRIPT_DIR}/runner.sh" "${runner_options[@]}" 2>&1 | tee "${run_dir}/run.log"; then
        echo "[${index}/${TOTAL_RUNS}] ${run_name}: complete"
        ((completed += 1))
    else
        echo "[${index}/${TOTAL_RUNS}] ${run_name}: failed" >&2
        ((failures += 1))
    fi
done

echo "parallel-worker sweep: ${completed} completed, ${skipped} skipped, ${failures} failed"
if ((failures > 0)); then
    exit 1
fi
