#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "sweep-common.sh must be sourced, not executed" >&2
    exit 2
fi

readonly SWEEP_TEST_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd
)"
readonly SWEEP_COMMON_SCRIPT="$(
    cd -- "${SWEEP_TEST_DIR}/../.." && pwd
)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${SWEEP_COMMON_SCRIPT}"

readonly SWEEP_GPT2_TEST_DIR="${SWEEP_TEST_DIR}/../sculptor-gpt2-8x8"
readonly SWEEP_CONFIGURATION_MANIFEST="$(
    realpath -m -- "${MITTENS_GPT2_SWEEP_CONFIGURATION_MANIFEST:-${SWEEP_TEST_DIR}/configurations.tsv}"
)"
readonly SWEEP_OUTPUT_ROOT="$(
    realpath -m -- "${MITTENS_GPT2_SWEEP_OUTPUT_ROOT:-${BUILD_ROOT}/tests/sculptor-gpt2-scheduling-sweep}"
)"
readonly SWEEP_RESULTS="${SWEEP_OUTPUT_ROOT}/results.csv"
readonly SWEEP_TOKEN_SELECTION="${MITTENS_GPT2_SWEEP_TOKENS:-4 8 16 32}"
readonly SWEEP_MODE_SELECTION="${MITTENS_GPT2_SWEEP_MODES:-analog digital}"
readonly SWEEP_CONFIGURATION_SELECTION="$(
    printf '%s' "${MITTENS_GPT2_SWEEP_CONFIGS:-all}"
)"
readonly SWEEP_FORCE_BUILD="${MITTENS_GPT2_SWEEP_FORCE_BUILD:-0}"
readonly SWEEP_FORCE_RUN="${MITTENS_GPT2_SWEEP_FORCE_RUN:-0}"
readonly SWEEP_KEEP_IR="${MITTENS_GPT2_SWEEP_KEEP_IR:-0}"
readonly SWEEP_REDUCTION_WIDTH=2
readonly SWEEP_RANDOM_SEED=0
readonly SWEEP_CPU_ISSUE_WIDTH=2
readonly SWEEP_CPU_CLOCK="1GHz"
readonly SWEEP_MEMORY_BACKEND="native"
readonly SWEEP_DIGITAL_CLOCK_GHZ="1.0"
readonly SWEEP_DIGITAL_VECTOR_BITS_PER_CYCLE=256
readonly SWEEP_ANALOG_MVM_LATENCY_NS="$(
    printf '%s' "${MITTENS_GPT2_SWEEP_ANALOG_MVM_LATENCY_NS:-100}"
)"
readonly SWEEP_ANALOG_COMPUTE_LATENCY_CYCLES="$(
    printf '%s' \
        "${MITTENS_GPT2_SWEEP_ANALOG_COMPUTE_LATENCY_CYCLES:-100}"
)"
readonly SWEEP_CORE_REGALLOC="basic"
readonly SWEEP_PIPELINE_VERSION="backend-aware-mvm100-v1"
readonly SWEEP_TIMING_OPTIONS="analog-mvm-latency-ns=${SWEEP_ANALOG_MVM_LATENCY_NS} analog-io-bits-per-cycle=256 analog-io-shared=true digital-clock-ghz=${SWEEP_DIGITAL_CLOCK_GHZ} digital-issue-width=${SWEEP_CPU_ISSUE_WIDTH} digital-vector-bits-per-cycle=${SWEEP_DIGITAL_VECTOR_BITS_PER_CYCLE} network-link-bits-per-cycle=32 network-hop-latency-cycles=10 network-pipelined=true"

sweep_timing_options_for_mode() {
    local mode="$1"

    case "${mode}" in
        analog|digital)
            printf '%s mvm-cost-mode=%s' "${SWEEP_TIMING_OPTIONS}" "${mode}"
            ;;
        *)
            echo "unknown placement cost mode: ${mode}" >&2
            return 1
            ;;
    esac
}

sweep_configuration_rows() {
    awk -F '\t' '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ {
            next
        }
        NF != 4 {
            printf "invalid configuration row %d: expected 4 fields\n", NR \
                >"/dev/stderr"
            exit 1
        }
        {
            print
        }
    ' "${SWEEP_CONFIGURATION_MANIFEST}"
}

sweep_item_selected() {
    local item="$1"
    local selection="$2"
    local selected

    if [[ "${selection}" == "all" ]]; then
        return 0
    fi
    for selected in ${selection}; do
        if [[ "${selected}" == "${item}" ]]; then
            return 0
        fi
    done
    return 1
}

sweep_validate_settings() {
    local item
    local configuration_count

    require_file "${SWEEP_CONFIGURATION_MANIFEST}"
    require_file "${SWEEP_GPT2_TEST_DIR}/gpt2_small.py"
    require_file "${SWEEP_GPT2_TEST_DIR}/run-deployment.sh"

    configuration_count="$(sweep_configuration_rows | wc -l)"
    if [[ "${configuration_count}" -lt 1 ]]; then
        echo "expected at least one scheduling configuration" >&2
        return 1
    fi
    if [[ "$(
        sweep_configuration_rows |
            cut -f1 |
            sort |
            uniq -d |
            wc -l
    )" -ne 0 ]]; then
        echo "scheduling configuration names must be unique" >&2
        return 1
    fi

    for item in ${SWEEP_TOKEN_SELECTION}; do
        case "${item}" in
            4|8|16|32) ;;
            *)
                echo "unsupported GPT-2 sweep token length: ${item}" >&2
                return 1
                ;;
        esac
    done
    for item in ${SWEEP_MODE_SELECTION}; do
        case "${item}" in
            analog|digital) ;;
            *)
                echo "unsupported GPT-2 sweep compute mode: ${item}" >&2
                return 1
                ;;
        esac
    done
    if [[ "${SWEEP_CONFIGURATION_SELECTION}" != "all" ]]; then
        for item in ${SWEEP_CONFIGURATION_SELECTION}; do
            if ! sweep_configuration_rows |
                cut -f1 |
                grep -Fx -- "${item}" >/dev/null; then
                echo "unknown GPT-2 sweep configuration: ${item}" >&2
                return 1
            fi
        done
    fi
    for item in \
        "${SWEEP_FORCE_BUILD}" \
        "${SWEEP_FORCE_RUN}" \
        "${SWEEP_KEEP_IR}"; do
        if [[ "${item}" != 0 && "${item}" != 1 ]]; then
            echo "sweep Boolean controls must be 0 or 1" >&2
            return 1
        fi
    done
    for item in \
        "${SWEEP_ANALOG_MVM_LATENCY_NS}" \
        "${SWEEP_ANALOG_COMPUTE_LATENCY_CYCLES}"; do
        if [[ ! "${item}" =~ ^[1-9][0-9]*$ ]]; then
            echo "analog MVM latency must be a positive integer" >&2
            return 1
        fi
    done
}

sweep_configuration_directory() {
    local token_count="$1"
    local configuration="$2"

    printf '%s/tokens-%s/configurations/%s' \
        "${SWEEP_OUTPUT_ROOT}" \
        "${token_count}" \
        "${configuration}"
}

sweep_mode_directory() {
    local token_count="$1"
    local configuration="$2"
    local mode="$3"

    printf '%s/%s' \
        "$(sweep_configuration_directory \
            "${token_count}" \
        "${configuration}")" \
        "${mode}"
}

sweep_selected_deployment_count() {
    local token_count
    local name
    local schedule
    local heuristic
    local balanced
    local mode
    local count=0

    for token_count in ${SWEEP_TOKEN_SELECTION}; do
        while IFS=$'\t' read -r name schedule heuristic balanced; do
            if ! sweep_item_selected \
                "${name}" \
                "${SWEEP_CONFIGURATION_SELECTION}"; then
                continue
            fi
            for mode in ${SWEEP_MODE_SELECTION}; do
                count=$((count + 1))
            done
        done < <(sweep_configuration_rows)
    done
    printf '%s' "${count}"
}

sweep_count_selected_mode_marker() {
    local relative_marker="$1"
    local token_count
    local name
    local schedule
    local heuristic
    local balanced
    local mode
    local mode_dir
    local count=0

    for token_count in ${SWEEP_TOKEN_SELECTION}; do
        while IFS=$'\t' read -r name schedule heuristic balanced; do
            if ! sweep_item_selected \
                "${name}" \
                "${SWEEP_CONFIGURATION_SELECTION}"; then
                continue
            fi
            for mode in ${SWEEP_MODE_SELECTION}; do
                mode_dir="$(
                    sweep_mode_directory \
                        "${token_count}" \
                        "${name}" \
                        "${mode}"
                )"
                if [[ -f "${mode_dir}/${relative_marker}" ]]; then
                    count=$((count + 1))
                fi
            done
        done < <(sweep_configuration_rows)
    done
    printf '%s' "${count}"
}

sweep_format_elapsed() {
    local elapsed_seconds="$1"
    local hours=$((elapsed_seconds / 3600))
    local minutes=$(((elapsed_seconds % 3600) / 60))
    local seconds=$((elapsed_seconds % 60))

    printf '%02d:%02d:%02d' "${hours}" "${minutes}" "${seconds}"
}

sweep_print_progress() {
    local phase="$1"
    local ordinal="$2"
    local total="$3"
    local token_count="$4"
    local configuration="$5"
    local mode="$6"
    local completed="$7"
    local failed="$8"
    local started_at="$9"
    local completed_label="${10}"
    local remaining=$((total - completed))
    local elapsed

    if [[ "${remaining}" -lt 0 ]]; then
        remaining=0
    fi
    elapsed="$(sweep_format_elapsed "$((SECONDS - started_at))")"
    printf '\n[%s %d/%d] tokens=%s config=%s mode=%s\n' \
        "${phase}" \
        "${ordinal}" \
        "${total}" \
        "${token_count}" \
        "${configuration}" \
        "${mode}"
    printf '%s: %d | Failed: %d | Remaining: %d | Elapsed: %s\n' \
        "${completed_label}" \
        "${completed}" \
        "${failed}" \
        "${remaining}" \
        "${elapsed}"
}

sweep_print_final_progress() {
    local phase="$1"
    local total="$2"
    local completed="$3"
    local failed="$4"
    local started_at="$5"
    local completed_label="$6"
    local state="${7:-COMPLETE}"
    local remaining=$((total - completed))
    local elapsed

    if [[ "${remaining}" -lt 0 ]]; then
        remaining=0
    fi
    elapsed="$(sweep_format_elapsed "$((SECONDS - started_at))")"
    printf '\n[%s %s] %s: %d | Failed: %d | Remaining: %d | Elapsed: %s\n' \
        "${phase}" \
        "${state}" \
        "${completed_label}" \
        "${completed}" \
        "${failed}" \
        "${remaining}" \
        "${elapsed}"
}
