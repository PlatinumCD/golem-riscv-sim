#!/usr/bin/env bash
set -euo pipefail

# Experiment 2: Placement strategy comparison

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Shared experiment parameters

# shellcheck source=params.sh
source "${SCRIPT_DIR}/params.sh"

# Baseline parameter overrides

GOLEM_MODEL_MEMORY_BACKEND=memhierarchy
SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE=8388608
SCULPTOR_NETWORK_WORD_BITS=32
SCULPTOR_DUPLICATE_MATRICES=false
GPT2_NUM_DECODERS=12
GPT2_SEQUENCE_LENGTH=16
SCULPTOR_GREEDY_TILE_ORDER=priority
SCULPTOR_GREEDY_PRIORITY_MODE=max
SCULPTOR_GREEDY_CANDIDATE_SCOPE=frontier
SCULPTOR_GREEDY_LOOKAHEAD=3
SCULPTOR_RANDOM_SEED=0

export GOLEM_MODEL_MEMORY_BACKEND
export SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE
export SCULPTOR_NETWORK_WORD_BITS
export SCULPTOR_DUPLICATE_MATRICES
export GPT2_NUM_DECODERS
export GPT2_SEQUENCE_LENGTH
export SCULPTOR_GREEDY_TILE_ORDER
export SCULPTOR_GREEDY_PRIORITY_MODE
export SCULPTOR_GREEDY_CANDIDATE_SCOPE
export SCULPTOR_GREEDY_LOOKAHEAD
export SCULPTOR_RANDOM_SEED

usage() {
    cat >&2 <<EOF
usage: $0 [--strategy NAME] [--worker N]
  --strategy NAME  Run only communication-aware or random placement.
  --worker N       Run only the configured trial for digital worker count N.
EOF
}

readonly ALL_STRATEGIES=(communication-aware random)
readonly ALL_DIGITAL_WORKER_VALUES=(1 2 4 6 8 10 12 14 16)
strategy_filter=""
worker_filter=""

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --strategy)
            if [[ "$#" -lt 2 ]]; then
                echo "--strategy requires a value" >&2
                usage
                exit 2
            fi
            strategy_filter="$2"
            shift 2
            ;;
        --strategy=*)
            strategy_filter="${1#*=}"
            shift
            ;;
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

STRATEGIES=("${ALL_STRATEGIES[@]}")
if [[ -n "${strategy_filter}" ]]; then
    valid_strategy=false
    for strategy in "${ALL_STRATEGIES[@]}"; do
        if [[ "${strategy_filter}" == "${strategy}" ]]; then
            valid_strategy=true
            break
        fi
    done
    if [[ "${valid_strategy}" != true ]]; then
        echo "unsupported placement strategy: ${strategy_filter}" >&2
        printf 'supported placement strategies: %s\n' "${ALL_STRATEGIES[*]}" >&2
        exit 2
    fi
    STRATEGIES=("${strategy_filter}")
fi
readonly STRATEGIES

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

readonly OUTPUT_ROOT="${GOLEM_PLACEMENT_EXPERIMENT_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/experiment-2-placement}"

# Run the placement comparison

mkdir -p -- "${OUTPUT_ROOT}"

remove_incomplete_trial() {
    local strategy="$1"
    local digital_workers="$2"
    local strategy_dir="${OUTPUT_ROOT}/${strategy}"
    local trial_dir="${strategy_dir}/workers-${digital_workers}"
    local strategy_dir_real
    local trial_dir_real

    if [[ ! -e "${trial_dir}" && ! -L "${trial_dir}" ]]; then
        return
    fi
    if [[ -L "${trial_dir}" || ! -d "${trial_dir}" ]]; then
        echo "refusing to delete an invalid trial directory: ${trial_dir}" >&2
        exit 2
    fi

    strategy_dir_real="$(realpath -e -- "${strategy_dir}")"
    trial_dir_real="$(realpath -e -- "${trial_dir}")"
    if [[ "${trial_dir_real}" != "${strategy_dir_real}/workers-${digital_workers}" ]]; then
        echo "refusing to delete a trial outside the strategy directory: ${trial_dir_real}" >&2
        exit 2
    fi

    echo "deleting incomplete trial: ${trial_dir}"
    find "${trial_dir_real}" -depth -delete
}

completed=0
skipped=0
failures=0
index=0
readonly TOTAL_RUNS="$(( ${#STRATEGIES[@]} * ${#DIGITAL_WORKER_VALUES[@]} ))"

for strategy in "${STRATEGIES[@]}"; do
    case "${strategy}" in
        communication-aware)
            placement_schedule=greedy
            ;;
        random)
            placement_schedule=random
            ;;
    esac

    strategy_dir="${OUTPUT_ROOT}/${strategy}"
    mkdir -p -- "${strategy_dir}"

    for digital_workers in "${DIGITAL_WORKER_VALUES[@]}"; do
        ((index += 1))
        run_dir="${strategy_dir}/workers-${digital_workers}"

        if [[ -f "${run_dir}/.complete" ]]; then
            echo "[${index}/${TOTAL_RUNS}] ${strategy}/workers-${digital_workers}: already complete"
            ((skipped += 1))
            continue
        fi

        remove_incomplete_trial "${strategy}" "${digital_workers}"

        require_change=true
        if [[ "${digital_workers}" -eq 1 ]]; then
            require_change=false
        fi

        echo "[${index}/${TOTAL_RUNS}] ${strategy}/workers-${digital_workers}: running"
        if GOLEM_EXPERIMENT_OUTPUT_DIR="${run_dir}" \
           SCULPTOR_PARALLEL_WORKERS="${digital_workers}" \
           SCULPTOR_REQUIRE_CHANGE="${require_change}" \
           SCULPTOR_PLACEMENT_SCHEDULE="${placement_schedule}" \
           "${SCRIPT_DIR}/runner.sh" \
               --run --trace --remove-intermediate-mlir; then
            echo "[${index}/${TOTAL_RUNS}] ${strategy}/workers-${digital_workers}: complete"
            ((completed += 1))
        else
            echo "[${index}/${TOTAL_RUNS}] ${strategy}/workers-${digital_workers}: failed" >&2
            ((failures += 1))
        fi

        if ((index < TOTAL_RUNS)); then
            echo "waiting 30 seconds before the next trial"
            sleep 30
        fi
    done
done

echo "experiment-2-placement: ${completed} completed, ${skipped} skipped, ${failures} failed"
if ((failures > 0)); then
    exit 1
fi
