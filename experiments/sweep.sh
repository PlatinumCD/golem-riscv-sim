#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=params.sh
source "${SCRIPT_DIR}/params.sh"

usage() {
    cat >&2 <<EOF
usage: $0 [--run] [--trace] [--remove-intermediate-mlir]
  --run                       Run each SST simulation after its build.
  --trace                     Record detailed trace files for each simulation.
  --remove-intermediate-mlir  Remove all generated MLIR files after success.
EOF
}

RUN_EXPERIMENT=false
TRACE_EXPERIMENT=false
REMOVE_INTERMEDIATE_MLIR=false
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --run)
            RUN_EXPERIMENT=true
            shift
            ;;
        --trace)
            TRACE_EXPERIMENT=true
            shift
            ;;
        --remove-intermediate-mlir)
            REMOVE_INTERMEDIATE_MLIR=true
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
readonly TRACE_EXPERIMENT
readonly REMOVE_INTERMEDIATE_MLIR

if [[ "${TRACE_EXPERIMENT}" == true && "${RUN_EXPERIMENT}" != true ]]; then
    echo "--trace requires --run" >&2
    exit 2
fi

readonly SWEEP_ROOT="${GOLEM_SWEEP_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/memhier-no-annealing}"
readonly TOTAL_RUNS=$((${#EXPERIMENT_PARALLEL_WORKER_VALUES[@]} * \
    ${#EXPERIMENT_GPT2_TOKEN_VALUES[@]} * \
    ${#EXPERIMENT_NETWORK_WORD_BITS_VALUES[@]}))
runner_options=()
if [[ "${RUN_EXPERIMENT}" == true ]]; then
    runner_options+=(--run)
fi
if [[ "${TRACE_EXPERIMENT}" == true ]]; then
    runner_options+=(--trace)
fi
if [[ "${REMOVE_INTERMEDIATE_MLIR}" == true ]]; then
    runner_options+=(--remove-intermediate-mlir)
fi

mkdir -p -- "${SWEEP_ROOT}"

failures=0
completed=0
skipped=0
index=0

for parallel_workers in "${EXPERIMENT_PARALLEL_WORKER_VALUES[@]}"; do
    for gpt2_tokens in "${EXPERIMENT_GPT2_TOKEN_VALUES[@]}"; do
        for network_word_bits in "${EXPERIMENT_NETWORK_WORD_BITS_VALUES[@]}"; do
            ((index += 1))
            run_name="workers-${parallel_workers}/tokens-${gpt2_tokens}/link-width-${network_word_bits}"
            run_dir="${SWEEP_ROOT}/${run_name}"
            completion_marker="${run_dir}/.complete"

            completion_is_valid=false
            if [[ -f "${completion_marker}" ]]; then
                if [[ "${RUN_EXPERIMENT}" == false || -f "${run_dir}/result.csv" ]]; then
                    completion_is_valid=true
                fi
            fi
            if [[ "${completion_is_valid}" == true && "${GOLEM_SWEEP_FORCE:-false}" != true ]]; then
                if [[ "${REMOVE_INTERMEDIATE_MLIR}" == true ]]; then
                    if ! "${SCRIPT_DIR}/remove-intermediate-mlir.sh" "${run_dir}"; then
                        echo "[${index}/${TOTAL_RUNS}] ${run_name}: failed to remove intermediate MLIR" >&2
                        ((failures += 1))
                        continue
                    fi
                    echo "[${index}/${TOTAL_RUNS}] ${run_name}: already complete and intermediate MLIR removed"
                else
                    echo "[${index}/${TOTAL_RUNS}] ${run_name}: already complete"
                fi
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
               GPT2_NUM_DECODERS="${EXPERIMENT_GPT2_DECODERS}" \
               GPT2_SEQUENCE_LENGTH="${gpt2_tokens}" \
               SCULPTOR_DIGITAL_ISSUE_WIDTH="${EXPERIMENT_DIGITAL_ISSUE_WIDTH}" \
               SCULPTOR_NETWORK_WORD_BITS="${network_word_bits}" \
               "${SCRIPT_DIR}/runner.sh" "${runner_options[@]}" 2>&1 | tee "${run_dir}/run.log"; then
                echo "[${index}/${TOTAL_RUNS}] ${run_name}: complete"
                ((completed += 1))
            else
                echo "[${index}/${TOTAL_RUNS}] ${run_name}: failed" >&2
                ((failures += 1))
            fi
        done
    done
done

echo "memhier-no-annealing sweep: ${completed} completed, ${skipped} skipped, ${failures} failed"
if ((failures > 0)); then
    exit 1
fi
