#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=factorial-common.sh
source "${SCRIPT_DIR}/factorial-common.sh"

readonly DEPLOYMENT_RUNNER="${FACTORIAL_GPT2_TEST_DIR}/run-deployment.sh"
readonly SST_THREADS="${MITTENS_GPT2_FACTORIAL_SST_THREADS:-1}"

factorial_validate_settings
require_executable "${DEPLOYMENT_RUNNER}"
if [[ ! "${SST_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_FACTORIAL_SST_THREADS must be a positive integer" >&2
    exit 1
fi
mapfile -t FACTORIAL_CONFIGURATION_ROWS < <(factorial_configuration_rows)
readonly -a FACTORIAL_CONFIGURATION_ROWS

prune_successful_deployment() {
    local configuration_dir="$1"
    local deployment_dir="${configuration_dir}/deployment"

    if [[ "${FACTORIAL_PRUNE_SUCCESSES}" == 0 ]]; then
        return
    fi
    find "${configuration_dir}/cores" -maxdepth 1 -type f \
        -name '*.o' -delete
    find "${deployment_dir}" -maxdepth 1 -type f \
        \( -name '*.o' -o -name '*.elf' \) -delete
}

run_deployment() {
    local token_count="$1"
    local configuration="$2"
    local configuration_dir
    local deployment_dir
    local pass_marker
    local failure_marker
    local runner_log
    local temporary_runner_log
    local passed_count
    local failed_count

    configuration_dir="$(
        factorial_configuration_directory \
            "${token_count}" \
            "${configuration}"
    )"
    deployment_dir="${configuration_dir}/deployment"
    pass_marker="${deployment_dir}/status.pass"
    failure_marker="${deployment_dir}/status.failed"

    require_file "${configuration_dir}/.complete"
    require_file "${configuration_dir}/active-cores.txt"
    if [[ "${FACTORIAL_FORCE_RUN}" == 0 && -f "${pass_marker}" ]]; then
        echo "[tokens ${token_count} / ${configuration}] already passed"
        return
    fi

    echo "[tokens ${token_count} / ${configuration}] foreground simulation"
    mkdir -p -- "${deployment_dir}"
    rm -f -- "${pass_marker}" "${failure_marker}"
    runner_log="${deployment_dir}/runner.log"
    temporary_runner_log="${runner_log}.tmp"
    rm -f -- "${temporary_runner_log}"
    if MITTENS_GPT2_LOWERING_DIR="${configuration_dir}" \
       MITTENS_GPT2_DEPLOYMENT_DIR="${deployment_dir}" \
       MITTENS_GPT2_SEQUENCE_LENGTH="${token_count}" \
       MITTENS_GPT2_CPU_ISSUE_WIDTH="${FACTORIAL_CPU_ISSUE_WIDTH}" \
       MITTENS_GPT2_CPU_CLOCK="${FACTORIAL_CPU_CLOCK}" \
       MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES="${FACTORIAL_ANALOG_COMPUTE_LATENCY_CYCLES}" \
       MITTENS_GPT2_MESH_WIDTH="${FACTORIAL_MESH_WIDTH}" \
       MITTENS_GPT2_MESH_HEIGHT="${FACTORIAL_MESH_HEIGHT}" \
       MITTENS_GPT2_SST_THREADS="${SST_THREADS}" \
       MITTENS_GPT2_VERBOSITY=0 \
       MITTENS_GPT2_PROFILE_MODE=summary \
       MITTENS_GPT2_TRANSMIT_POLICY=async \
       MITTENS_MEMORY_BACKEND="${FACTORIAL_MEMORY_BACKEND}" \
        "${DEPLOYMENT_RUNNER}" \
        >"${temporary_runner_log}" 2>&1; then
        mv -- "${temporary_runner_log}" "${runner_log}"
        grep -E \
            'Simulation is complete|Mittens profile:|GPT-2 fixture .*: PASS' \
            "${runner_log}" || true
        printf '%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"${pass_marker}"
        "${SCRIPT_DIR}/summarize-results.sh"
        prune_successful_deployment "${configuration_dir}"
        return
    fi

    mv -- "${temporary_runner_log}" "${runner_log}"
    echo "deployment failed; final diagnostics follow: ${runner_log}" >&2
    tail -n 160 -- "${runner_log}" >&2
    printf '%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"${failure_marker}"
    "${SCRIPT_DIR}/summarize-results.sh"
    passed_count="$(
        factorial_count_selected_marker 'deployment/status.pass'
    )"
    failed_count="$(
        factorial_count_selected_marker 'deployment/status.failed'
    )"
    factorial_print_final_progress \
        SIM \
        "${SIM_PROGRESS_TOTAL}" \
        "${passed_count}" \
        "${failed_count}" \
        "${SIM_PROGRESS_STARTED_AT}" \
        STOPPED
    echo "GPT-2 factorial stopped at tokens=${token_count}, configuration=${configuration}" >&2
    return 1
}

readonly SIM_PROGRESS_TOTAL="$(factorial_selected_count)"
readonly SIM_PROGRESS_STARTED_AT="${SECONDS}"
SIM_PROGRESS_ORDINAL=0

for token_count in ${FACTORIAL_TOKEN_SELECTION}; do
    for configuration_row in "${FACTORIAL_CONFIGURATION_ROWS[@]}"; do
        IFS=$'\t' read -r \
            name \
            boundary_regret \
            compact_region \
            link_pressure \
            balanced_reductions \
            distributed_matmul \
            heuristic \
            <<<"${configuration_row}"
        if ! factorial_item_selected \
            "${name}" \
            "${FACTORIAL_CONFIGURATION_SELECTION}"; then
            continue
        fi
        SIM_PROGRESS_ORDINAL=$((SIM_PROGRESS_ORDINAL + 1))
        passed_count="$(
            factorial_count_selected_marker 'deployment/status.pass'
        )"
        failed_count="$(
            factorial_count_selected_marker 'deployment/status.failed'
        )"
        factorial_print_progress \
            SIM \
            "${SIM_PROGRESS_ORDINAL}" \
            "${SIM_PROGRESS_TOTAL}" \
            "${token_count}" \
            "${name}" \
            "${passed_count}" \
            "${failed_count}" \
            "${SIM_PROGRESS_STARTED_AT}"
        run_deployment "${token_count}" "${name}"
    done
done

"${SCRIPT_DIR}/summarize-results.sh"
"${SCRIPT_DIR}/analyze-results.py" "${FACTORIAL_RESULTS}" "${FACTORIAL_OUTPUT_ROOT}"
passed_count="$(factorial_count_selected_marker 'deployment/status.pass')"
failed_count="$(factorial_count_selected_marker 'deployment/status.failed')"
factorial_print_final_progress \
    SIM \
    "${SIM_PROGRESS_TOTAL}" \
    "${passed_count}" \
    "${failed_count}" \
    "${SIM_PROGRESS_STARTED_AT}"
echo "GPT-2 factorial: PASS"
