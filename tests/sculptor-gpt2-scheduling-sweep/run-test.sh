#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sweep-common.sh
source "${SCRIPT_DIR}/sweep-common.sh"

readonly DEPLOYMENT_RUNNER="${SWEEP_GPT2_TEST_DIR}/run-deployment.sh"
readonly SST_THREADS="${MITTENS_GPT2_SWEEP_SST_THREADS:-1}"

sweep_validate_settings
require_executable "${DEPLOYMENT_RUNNER}"
if [[ ! "${SST_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_SWEEP_SST_THREADS must be a positive integer" >&2
    exit 1
fi
mapfile -t SWEEP_CONFIGURATION_ROWS < <(sweep_configuration_rows)
readonly -a SWEEP_CONFIGURATION_ROWS

run_deployment() {
    local token_count="$1"
    local configuration="$2"
    local mode="$3"
    local mode_dir
    local deployment_dir
    local pass_marker
    local failure_marker
    local passed_count
    local failed_count

    mode_dir="$(
        sweep_mode_directory "${token_count}" "${configuration}" "${mode}"
    )"
    deployment_dir="${mode_dir}/deployment"
    pass_marker="${deployment_dir}/status.pass"
    failure_marker="${deployment_dir}/status.failed"

    require_file "${mode_dir}/.complete"
    require_file "${mode_dir}/active-cores.txt"
    if [[ "${SWEEP_FORCE_RUN}" == 0 && -f "${pass_marker}" ]]; then
        echo "[tokens ${token_count} / ${configuration} / ${mode}] already passed"
        return
    fi

    echo "[tokens ${token_count} / ${configuration} / ${mode}] foreground simulation"
    mkdir -p -- "${deployment_dir}"
    rm -f -- "${pass_marker}" "${failure_marker}"
    if MITTENS_GPT2_LOWERING_DIR="${mode_dir}" \
       MITTENS_GPT2_DEPLOYMENT_DIR="${deployment_dir}" \
       MITTENS_GPT2_SEQUENCE_LENGTH="${token_count}" \
       MITTENS_GPT2_CPU_ISSUE_WIDTH="${SWEEP_CPU_ISSUE_WIDTH}" \
       MITTENS_GPT2_CPU_CLOCK="${SWEEP_CPU_CLOCK}" \
       MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES="${SWEEP_ANALOG_COMPUTE_LATENCY_CYCLES}" \
       MITTENS_GPT2_SST_THREADS="${SST_THREADS}" \
       MITTENS_GPT2_VERBOSITY=1 \
       MITTENS_MEMORY_BACKEND="${SWEEP_MEMORY_BACKEND}" \
        "${DEPLOYMENT_RUNNER}"; then
        printf '%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"${pass_marker}"
        "${SCRIPT_DIR}/summarize-results.sh"
        return
    fi

    printf '%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"${failure_marker}"
    "${SCRIPT_DIR}/summarize-results.sh"
    if [[ "${SWEEP_FORCE_RUN}" == 1 ]]; then
        passed_count=$((SIM_PROGRESS_ORDINAL - 1))
        failed_count=1
    else
        passed_count="$(
            sweep_count_selected_mode_marker \
                'deployment/status.pass'
        )"
        failed_count="$(
            sweep_count_selected_mode_marker \
                'deployment/status.failed'
        )"
    fi
    sweep_print_final_progress \
        SIM \
        "${SIM_PROGRESS_TOTAL}" \
        "${passed_count}" \
        "${failed_count}" \
        "${SIM_PROGRESS_STARTED_AT}" \
        Passed \
        STOPPED
    echo "GPT-2 scheduling sweep stopped at tokens=${token_count}, configuration=${configuration}, mode=${mode}" >&2
    return 1
}

readonly SIM_PROGRESS_TOTAL="$(sweep_selected_deployment_count)"
readonly SIM_PROGRESS_STARTED_AT="${SECONDS}"
SIM_PROGRESS_ORDINAL=0

for token_count in ${SWEEP_TOKEN_SELECTION}; do
    for configuration_row in "${SWEEP_CONFIGURATION_ROWS[@]}"; do
        IFS=$'\t' read -r name schedule heuristic balanced \
            <<<"${configuration_row}"
        if ! sweep_item_selected \
            "${name}" \
            "${SWEEP_CONFIGURATION_SELECTION}"; then
            continue
        fi
        for mode in ${SWEEP_MODE_SELECTION}; do
            SIM_PROGRESS_ORDINAL=$((SIM_PROGRESS_ORDINAL + 1))
            if [[ "${SWEEP_FORCE_RUN}" == 1 ]]; then
                passed_count=$((SIM_PROGRESS_ORDINAL - 1))
                failed_count=0
            else
                passed_count="$(
                    sweep_count_selected_mode_marker \
                        'deployment/status.pass'
                )"
                failed_count="$(
                    sweep_count_selected_mode_marker \
                        'deployment/status.failed'
                )"
            fi
            sweep_print_progress \
                SIM \
                "${SIM_PROGRESS_ORDINAL}" \
                "${SIM_PROGRESS_TOTAL}" \
                "${token_count}" \
                "${name}" \
                "${mode}" \
                "${passed_count}" \
                "${failed_count}" \
                "${SIM_PROGRESS_STARTED_AT}" \
                Passed
            run_deployment "${token_count}" "${name}" "${mode}"
        done
    done
done

"${SCRIPT_DIR}/summarize-results.sh"
if [[ "${SWEEP_FORCE_RUN}" == 1 ]]; then
    passed_count="${SIM_PROGRESS_TOTAL}"
    failed_count=0
else
    passed_count="$(
        sweep_count_selected_mode_marker 'deployment/status.pass'
    )"
    failed_count="$(
        sweep_count_selected_mode_marker 'deployment/status.failed'
    )"
fi
sweep_print_final_progress \
    SIM \
    "${SIM_PROGRESS_TOTAL}" \
    "${passed_count}" \
    "${failed_count}" \
    "${SIM_PROGRESS_STARTED_AT}" \
    Passed
echo "GPT-2 scheduling sweep: PASS"
