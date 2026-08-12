#!/usr/bin/env bash
set -euo pipefail

# Experiment 1: Baseline

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Shared experiment parameters

# shellcheck source=params.sh
source "${SCRIPT_DIR}/params.sh"

# Experiment-specific parameter overrides

GOLEM_MODEL_MEMORY_BACKEND=memhierarchy
SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE=8388608
SCULPTOR_NETWORK_WORD_BITS=32
SCULPTOR_DUPLICATE_MATRICES=false
GPT2_NUM_DECODERS=12
GPT2_SEQUENCE_LENGTH=16
SCULPTOR_PLACEMENT_SCHEDULE=snake

export GOLEM_MODEL_MEMORY_BACKEND
export SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE
export SCULPTOR_NETWORK_WORD_BITS
export SCULPTOR_DUPLICATE_MATRICES
export GPT2_NUM_DECODERS
export GPT2_SEQUENCE_LENGTH
export SCULPTOR_PLACEMENT_SCHEDULE

usage() {
    cat >&2 <<EOF
usage: $0 [--worker N]
  --worker N  Run only the configured trial for digital worker count N.
EOF
}

readonly ALL_DIGITAL_WORKER_VALUES=(1 2 4 6 8 10 12 14 16)
worker_filter=""
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --worker)
            if [[ "$#" -lt 2 ]]; then
                echo "--worker requires a value" >&2
                usage
                exit 2
            fi
            worker_filter="$2"
            shift 2
            ;;
        --worker=*)
            worker_filter="${1#*=}"
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

DIGITAL_WORKER_VALUES=("${ALL_DIGITAL_WORKER_VALUES[@]}")
if [[ -n "${worker_filter}" ]]; then
    valid_worker=false
    for worker in "${ALL_DIGITAL_WORKER_VALUES[@]}"; do
        if [[ "${worker_filter}" == "${worker}" ]]; then
            valid_worker=true
            break
        fi
    done
    if [[ "${valid_worker}" != true ]]; then
        echo "unsupported worker count: ${worker_filter}" >&2
        printf 'supported worker counts: %s\n' "${ALL_DIGITAL_WORKER_VALUES[*]}" >&2
        exit 2
    fi
    DIGITAL_WORKER_VALUES=("${worker_filter}")
fi
readonly DIGITAL_WORKER_VALUES
readonly OUTPUT_ROOT="${GOLEM_BASELINE_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/experiment-1-baseline}"

# Run the baseline sweep

mkdir -p -- "${OUTPUT_ROOT}"

remove_incomplete_trial() {
    local digital_workers="$1"
    local trial_dir="${OUTPUT_ROOT}/workers-${digital_workers}"
    local output_root_real
    local trial_dir_real

    if [[ ! -e "${trial_dir}" && ! -L "${trial_dir}" ]]; then
        return
    fi
    if [[ -L "${trial_dir}" || ! -d "${trial_dir}" ]]; then
        echo "refusing to delete an invalid trial directory: ${trial_dir}" >&2
        exit 2
    fi

    output_root_real="$(realpath -e -- "${OUTPUT_ROOT}")"
    trial_dir_real="$(realpath -e -- "${trial_dir}")"
    if [[ "${trial_dir_real}" != "${output_root_real}/workers-${digital_workers}" ]]; then
        echo "refusing to delete a trial outside the output root: ${trial_dir_real}" >&2
        exit 2
    fi

    echo "deleting incomplete trial: ${trial_dir}"
    find "${trial_dir_real}" -depth -delete
}

completed=0
skipped=0
failures=0
index=0
readonly TOTAL_RUNS="${#DIGITAL_WORKER_VALUES[@]}"

for digital_workers in "${DIGITAL_WORKER_VALUES[@]}"; do
    ((index += 1))
    run_dir="${OUTPUT_ROOT}/workers-${digital_workers}"

    if [[ -f "${run_dir}/.complete" ]]; then
        echo "[${index}/${TOTAL_RUNS}] workers-${digital_workers}: already complete"
        ((skipped += 1))
        continue
    fi

    remove_incomplete_trial "${digital_workers}"

    require_change=true
    if [[ "${digital_workers}" -eq 1 ]]; then
        require_change=false
    fi

    echo "[${index}/${TOTAL_RUNS}] workers-${digital_workers}: running"
    if GOLEM_EXPERIMENT_OUTPUT_DIR="${run_dir}" \
       SCULPTOR_PARALLEL_WORKERS="${digital_workers}" \
       SCULPTOR_REQUIRE_CHANGE="${require_change}" \
       "${SCRIPT_DIR}/runner.sh" \
           --run --trace --remove-intermediate-mlir; then
        echo "[${index}/${TOTAL_RUNS}] workers-${digital_workers}: complete"
        ((completed += 1))
    else
        echo "[${index}/${TOTAL_RUNS}] workers-${digital_workers}: failed" >&2
        ((failures += 1))
    fi

    if ((index < TOTAL_RUNS)); then
        echo "waiting 30 seconds before the next trial"
        sleep 30
    fi
done

echo "experiment-1-baseline: ${completed} completed, ${skipped} skipped, ${failures} failed"
if ((failures > 0)); then
    exit 1
fi
