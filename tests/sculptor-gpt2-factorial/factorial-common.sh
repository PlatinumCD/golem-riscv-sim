#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "factorial-common.sh must be sourced, not executed" >&2
    exit 2
fi

readonly FACTORIAL_TEST_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd
)"
readonly FACTORIAL_COMMON_SCRIPT="$(
    cd -- "${FACTORIAL_TEST_DIR}/../.." && pwd
)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${FACTORIAL_COMMON_SCRIPT}"

readonly FACTORIAL_GPT2_TEST_DIR="${FACTORIAL_TEST_DIR}/../sculptor-gpt2-8x8"
readonly FACTORIAL_OUTPUT_ROOT="$(
    realpath -m -- "${MITTENS_GPT2_FACTORIAL_OUTPUT_ROOT:-${BUILD_ROOT}/tests/sculptor-gpt2-factorial}"
)"
readonly FACTORIAL_RESULTS="${FACTORIAL_OUTPUT_ROOT}/results.csv"
readonly FACTORIAL_ANALYSIS="${FACTORIAL_OUTPUT_ROOT}/analysis.md"
readonly FACTORIAL_TOKEN_SELECTION="${MITTENS_GPT2_FACTORIAL_TOKENS:-4 8 16 32}"
readonly FACTORIAL_CONFIGURATION_SELECTION="$(
    printf '%s' "${MITTENS_GPT2_FACTORIAL_CONFIGS:-all}"
)"
readonly FACTORIAL_FORCE_BUILD="${MITTENS_GPT2_FACTORIAL_FORCE_BUILD:-0}"
readonly FACTORIAL_FORCE_RUN="${MITTENS_GPT2_FACTORIAL_FORCE_RUN:-0}"
readonly FACTORIAL_KEEP_IR="${MITTENS_GPT2_FACTORIAL_KEEP_IR:-0}"
readonly FACTORIAL_PRUNE_SUCCESSES="${MITTENS_GPT2_FACTORIAL_PRUNE_SUCCESSES:-1}"
readonly FACTORIAL_REDUCTION_WIDTH=2
readonly FACTORIAL_CPU_ISSUE_WIDTH=2
readonly FACTORIAL_CPU_CLOCK="1GHz"
readonly FACTORIAL_DIGITAL_CLOCK_GHZ="1"
readonly FACTORIAL_DIGITAL_VECTOR_BITS_PER_CYCLE=256
readonly FACTORIAL_MEMORY_BACKEND="native"
readonly FACTORIAL_ISLAND_ASSIGNMENT="${MITTENS_GPT2_FACTORIAL_ISLAND_ASSIGNMENT:-legacy}"
readonly FACTORIAL_MESH_WIDTH=12
readonly FACTORIAL_MESH_HEIGHT=12
readonly FACTORIAL_ARRAYS_PER_CORE=4
readonly FACTORIAL_ANALOG_ARRAY_ROWS=1024
readonly FACTORIAL_ANALOG_ARRAY_COLUMNS=512
readonly FACTORIAL_ANALOG_MVM_LATENCY_NS="${MITTENS_GPT2_FACTORIAL_ANALOG_MVM_LATENCY_NS:-100}"
readonly FACTORIAL_ANALOG_COMPUTE_LATENCY_CYCLES="${MITTENS_GPT2_FACTORIAL_ANALOG_COMPUTE_LATENCY_CYCLES:-100}"
readonly FACTORIAL_CORE_REGALLOC="basic"
readonly FACTORIAL_PIPELINE_VERSION="gpt2-factorial-12x12-optimizer-all-separable-v4-${FACTORIAL_ISLAND_ASSIGNMENT}"
readonly FACTORIAL_TIMING_OPTIONS="mvm-cost-mode=analog analog-mvm-latency-ns=${FACTORIAL_ANALOG_MVM_LATENCY_NS} analog-io-bits-per-cycle=256 analog-io-shared=true digital-clock-ghz=${FACTORIAL_DIGITAL_CLOCK_GHZ} digital-issue-width=${FACTORIAL_CPU_ISSUE_WIDTH} digital-vector-bits-per-cycle=${FACTORIAL_DIGITAL_VECTOR_BITS_PER_CYCLE} fixed-runtime-dispatch-cycles=8 fixed-task-entry-cycles=4 fixed-task-exit-cycles=4 network-link-word-bits=32 network-link-bits-per-cycle=32 network-hop-latency-cycles=10 network-pipelined=true protocol-words-per-route=5 nic-injection-words-per-cycle=1 rx-dma-words-per-cycle=8 timing-boundary=warm runtime-task-policy=lowest-local-task-index runtime-transmit-policy=overlap-ready-tasks memory-backend=native-untimed routing-policy=xy"

factorial_configuration_rows() {
    local boundary_regret
    local compact_region
    local link_pressure
    local balanced_reductions
    local distributed_matmul
    local name
    local heuristic

    for boundary_regret in 0 1; do
        for compact_region in 0 1; do
            for link_pressure in 0 1; do
                for balanced_reductions in 0 1; do
                    for distributed_matmul in 0 1; do
                        name="br${boundary_regret}-cr${compact_region}-lp${link_pressure}-rb${balanced_reductions}-dm${distributed_matmul}"
                        heuristic="transfer-cost"
                        if [[ "${boundary_regret}" == 1 ]]; then
                            heuristic+=",boundary-regret"
                        fi
                        if [[ "${compact_region}" == 1 ]]; then
                            heuristic+=",compact-region"
                        fi
                        if [[ "${link_pressure}" == 1 ]]; then
                            heuristic+=",spatial-link-pressure"
                        fi
                        heuristic+=",lookahead=3,beam=8,scope=diagonal"
                        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                            "${name}" \
                            "${boundary_regret}" \
                            "${compact_region}" \
                            "${link_pressure}" \
                            "${balanced_reductions}" \
                            "${distributed_matmul}" \
                            "${heuristic}"
                    done
                done
            done
        done
    done
}

factorial_item_selected() {
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

factorial_validate_settings() {
    local item
    local configuration_count

    require_file "${FACTORIAL_GPT2_TEST_DIR}/gpt2_small.py"
    require_file "${FACTORIAL_GPT2_TEST_DIR}/run-deployment.sh"
    case "${FACTORIAL_ISLAND_ASSIGNMENT}" in
        legacy|multi-terminal-balanced) ;;
        *)
            echo "MITTENS_GPT2_FACTORIAL_ISLAND_ASSIGNMENT must be legacy or multi-terminal-balanced" >&2
            return 1
            ;;
    esac

    configuration_count="$(factorial_configuration_rows | wc -l)"
    if [[ "${configuration_count}" -ne 32 ]]; then
        echo "expected exactly 32 factorial configurations" >&2
        return 1
    fi
    if [[ "$(
        factorial_configuration_rows |
            cut -f1 |
            sort |
            uniq -d |
            wc -l
    )" -ne 0 ]]; then
        echo "factorial configuration names must be unique" >&2
        return 1
    fi

    for item in ${FACTORIAL_TOKEN_SELECTION}; do
        case "${item}" in
            4|8|16|32) ;;
            *)
                echo "unsupported GPT-2 token length: ${item}" >&2
                return 1
                ;;
        esac
    done
    if [[ "${FACTORIAL_CONFIGURATION_SELECTION}" != "all" ]]; then
        for item in ${FACTORIAL_CONFIGURATION_SELECTION}; do
            if ! factorial_configuration_rows |
                cut -f1 |
                grep -Fx -- "${item}" >/dev/null; then
                echo "unknown factorial configuration: ${item}" >&2
                return 1
            fi
        done
    fi
    for item in \
        "${FACTORIAL_FORCE_BUILD}" \
        "${FACTORIAL_FORCE_RUN}" \
        "${FACTORIAL_KEEP_IR}" \
        "${FACTORIAL_PRUNE_SUCCESSES}"; do
        if [[ "${item}" != 0 && "${item}" != 1 ]]; then
            echo "factorial Boolean controls must be 0 or 1" >&2
            return 1
        fi
    done
    for item in \
        "${FACTORIAL_ANALOG_MVM_LATENCY_NS}" \
        "${FACTORIAL_ANALOG_COMPUTE_LATENCY_CYCLES}"; do
        if [[ ! "${item}" =~ ^[1-9][0-9]*$ ]]; then
            echo "analog MVM latency must be a positive integer" >&2
            return 1
        fi
    done
}

factorial_configuration_directory() {
    local token_count="$1"
    local configuration="$2"

    printf '%s/tokens-%s/configurations/%s' \
        "${FACTORIAL_OUTPUT_ROOT}" \
        "${token_count}" \
        "${configuration}"
}

factorial_variant_directory() {
    local token_count="$1"
    local balanced_reductions="$2"
    local distributed_matmul="$3"

    printf '%s/tokens-%s/variants/rb%s-dm%s' \
        "${FACTORIAL_OUTPUT_ROOT}" \
        "${token_count}" \
        "${balanced_reductions}" \
        "${distributed_matmul}"
}

factorial_selected_count() {
    local token_count
    local name
    local boundary_regret
    local compact_region
    local link_pressure
    local balanced_reductions
    local distributed_matmul
    local heuristic
    local count=0

    for token_count in ${FACTORIAL_TOKEN_SELECTION}; do
        while IFS=$'\t' read -r \
            name \
            boundary_regret \
            compact_region \
            link_pressure \
            balanced_reductions \
            distributed_matmul \
            heuristic; do
            if factorial_item_selected \
                "${name}" \
                "${FACTORIAL_CONFIGURATION_SELECTION}"; then
                count=$((count + 1))
            fi
        done < <(factorial_configuration_rows)
    done
    printf '%s' "${count}"
}

factorial_count_selected_marker() {
    local relative_marker="$1"
    local token_count
    local name
    local boundary_regret
    local compact_region
    local link_pressure
    local balanced_reductions
    local distributed_matmul
    local heuristic
    local configuration_dir
    local count=0

    for token_count in ${FACTORIAL_TOKEN_SELECTION}; do
        while IFS=$'\t' read -r \
            name \
            boundary_regret \
            compact_region \
            link_pressure \
            balanced_reductions \
            distributed_matmul \
            heuristic; do
            if ! factorial_item_selected \
                "${name}" \
                "${FACTORIAL_CONFIGURATION_SELECTION}"; then
                continue
            fi
            configuration_dir="$(
                factorial_configuration_directory \
                    "${token_count}" \
                    "${name}"
            )"
            if [[ -f "${configuration_dir}/${relative_marker}" ]]; then
                count=$((count + 1))
            fi
        done < <(factorial_configuration_rows)
    done
    printf '%s' "${count}"
}

factorial_format_elapsed() {
    local elapsed_seconds="$1"
    local hours=$((elapsed_seconds / 3600))
    local minutes=$(((elapsed_seconds % 3600) / 60))
    local seconds=$((elapsed_seconds % 60))

    printf '%02d:%02d:%02d' "${hours}" "${minutes}" "${seconds}"
}

factorial_print_progress() {
    local phase="$1"
    local ordinal="$2"
    local total="$3"
    local token_count="$4"
    local configuration="$5"
    local completed="$6"
    local failed="$7"
    local started_at="$8"
    local remaining=$((total - completed))
    local elapsed

    if [[ "${remaining}" -lt 0 ]]; then
        remaining=0
    fi
    elapsed="$(factorial_format_elapsed "$((SECONDS - started_at))")"
    printf '\n[%s %d/%d] tokens=%s config=%s\n' \
        "${phase}" \
        "${ordinal}" \
        "${total}" \
        "${token_count}" \
        "${configuration}"
    printf 'Complete: %d | Failed: %d | Remaining: %d | Elapsed: %s\n' \
        "${completed}" \
        "${failed}" \
        "${remaining}" \
        "${elapsed}"
}

factorial_print_final_progress() {
    local phase="$1"
    local total="$2"
    local completed="$3"
    local failed="$4"
    local started_at="$5"
    local state="${6:-COMPLETE}"
    local remaining=$((total - completed))
    local elapsed

    if [[ "${remaining}" -lt 0 ]]; then
        remaining=0
    fi
    elapsed="$(factorial_format_elapsed "$((SECONDS - started_at))")"
    printf '\n[%s %s] Complete: %d | Failed: %d | Remaining: %d | Elapsed: %s\n' \
        "${phase}" \
        "${state}" \
        "${completed}" \
        "${failed}" \
        "${remaining}" \
        "${elapsed}"
}
