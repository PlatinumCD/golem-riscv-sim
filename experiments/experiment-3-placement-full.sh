#!/usr/bin/env bash
set -euo pipefail

# Experiment 3: Full placement

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

export GOLEM_MODEL_MEMORY_BACKEND
export SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE
export SCULPTOR_NETWORK_WORD_BITS
export SCULPTOR_DUPLICATE_MATRICES
export GPT2_NUM_DECODERS
export GPT2_SEQUENCE_LENGTH

usage() {
    cat >&2 <<EOF
usage: $0 [--placement NAME] [--worker N]
  --placement NAME  Run only random, snake, greedy-l3, greedy-l2, or greedy-l1.
  --worker N        Run only the configured trial for digital worker count N.
EOF
}

readonly ALL_PLACEMENTS=(random snake greedy-l3 greedy-l2 greedy-l1)
readonly ALL_DIGITAL_WORKER_VALUES=(1 2 4 6 8 10 12 14 16)
placement_filter=""
worker_filter=""
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --placement)
            if [[ "$#" -lt 2 ]]; then
                echo "--placement requires a value" >&2
                usage
                exit 2
            fi
            placement_filter="$2"
            shift 2
            ;;
        --placement=*)
            placement_filter="${1#*=}"
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

PLACEMENTS=("${ALL_PLACEMENTS[@]}")
if [[ -n "${placement_filter}" ]]; then
    valid_placement=false
    for placement in "${ALL_PLACEMENTS[@]}"; do
        if [[ "${placement_filter}" == "${placement}" ]]; then
            valid_placement=true
            break
        fi
    done
    if [[ "${valid_placement}" != true ]]; then
        echo "unsupported placement: ${placement_filter}" >&2
        printf 'supported placements: %s\n' "${ALL_PLACEMENTS[*]}" >&2
        exit 2
    fi
    PLACEMENTS=("${placement_filter}")
fi
readonly PLACEMENTS

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
readonly OUTPUT_ROOT="${GOLEM_PLACEMENT_FULL_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/experiment-3-placement-full}"

# Run the full-placement sweep

mkdir -p -- "${OUTPUT_ROOT}"

remove_incomplete_trial() {
    local placement="$1"
    local digital_workers="$2"
    local placement_dir="${OUTPUT_ROOT}/${placement}"
    local trial_dir="${placement_dir}/workers-${digital_workers}"
    local placement_dir_real
    local trial_dir_real

    if [[ ! -e "${trial_dir}" && ! -L "${trial_dir}" ]]; then
        return
    fi
    if [[ -L "${trial_dir}" || ! -d "${trial_dir}" ]]; then
        echo "refusing to delete an invalid trial directory: ${trial_dir}" >&2
        exit 2
    fi

    placement_dir_real="$(realpath -e -- "${placement_dir}")"
    trial_dir_real="$(realpath -e -- "${trial_dir}")"
    if [[ "${trial_dir_real}" != "${placement_dir_real}/workers-${digital_workers}" ]]; then
        echo "refusing to delete a trial outside the placement directory: ${trial_dir_real}" >&2
        exit 2
    fi

    echo "deleting incomplete trial: ${trial_dir}"
    find "${trial_dir_real}" -depth -delete
}

completed=0
skipped=0
failures=0
index=0
readonly TOTAL_RUNS="$(( ${#PLACEMENTS[@]} * ${#DIGITAL_WORKER_VALUES[@]} ))"

for placement in "${PLACEMENTS[@]}"; do
    case "${placement}" in
        random)
            placement_schedule=random
            greedy_lookahead=3
            ;;
        snake)
            placement_schedule=snake
            greedy_lookahead=3
            ;;
        greedy-l3)
            placement_schedule=greedy
            greedy_lookahead=3
            ;;
        greedy-l2)
            placement_schedule=greedy
            greedy_lookahead=2
            ;;
        greedy-l1)
            placement_schedule=greedy
            greedy_lookahead=1
            ;;
    esac

    placement_dir="${OUTPUT_ROOT}/${placement}"
    mkdir -p -- "${placement_dir}"

    for digital_workers in "${DIGITAL_WORKER_VALUES[@]}"; do
        ((index += 1))
        run_dir="${placement_dir}/workers-${digital_workers}"

        if [[ -f "${run_dir}/.complete" ]]; then
            echo "[${index}/${TOTAL_RUNS}] ${placement}/workers-${digital_workers}: already complete"
            ((skipped += 1))
            continue
        fi

        remove_incomplete_trial "${placement}" "${digital_workers}"
        mkdir -p -- "${run_dir}"
        driver_log="${run_dir}/driver.log"

        require_change=true
        if [[ "${digital_workers}" -eq 1 ]]; then
            require_change=false
        fi

        echo "[${index}/${TOTAL_RUNS}] ${placement}/workers-${digital_workers}: running"
        if GOLEM_EXPERIMENT_OUTPUT_DIR="${run_dir}" \
           SCULPTOR_PARALLEL_WORKERS="${digital_workers}" \
           SCULPTOR_REQUIRE_CHANGE="${require_change}" \
           SCULPTOR_PLACEMENT_SCHEDULE="${placement_schedule}" \
           SCULPTOR_GREEDY_LOOKAHEAD="${greedy_lookahead}" \
           "${SCRIPT_DIR}/runner.sh" \
               --run --trace --remove-intermediate-mlir \
               >"${driver_log}" 2>&1; then
            echo "[${index}/${TOTAL_RUNS}] ${placement}/workers-${digital_workers}: complete"
            ((completed += 1))
        else
            echo "[${index}/${TOTAL_RUNS}] ${placement}/workers-${digital_workers}: failed" >&2
            tail -n 40 "${driver_log}" >&2
            ((failures += 1))
        fi

        if ((index < TOTAL_RUNS)); then
            echo "waiting 30 seconds before the next trial"
            sleep 30
        fi
    done
done

echo "experiment-3-placement-full: ${completed} completed, ${skipped} skipped, ${failures} failed"
if ((failures > 0)); then
    exit 1
fi
