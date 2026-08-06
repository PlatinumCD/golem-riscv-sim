#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sweep-common.sh
source "${SCRIPT_DIR}/sweep-common.sh"

sweep_validate_settings
mkdir -p -- "${SWEEP_OUTPUT_ROOT}"
readonly TEMPORARY_RESULTS="${SWEEP_RESULTS}.tmp"

csv_string() {
    local value="${1//\"/\"\"}"
    printf '"%s"' "${value}"
}

simulation_time_ns() {
    local value="$1"
    local unit="$2"

    case "${unit}" in
        s) awk -v value="${value}" 'BEGIN { printf "%.0f", value * 1e9 }' ;;
        ms) awk -v value="${value}" 'BEGIN { printf "%.0f", value * 1e6 }' ;;
        us) awk -v value="${value}" 'BEGIN { printf "%.0f", value * 1e3 }' ;;
        ns) awk -v value="${value}" 'BEGIN { printf "%.0f", value }' ;;
        ps) awk -v value="${value}" 'BEGIN { printf "%.0f", value / 1e3 }' ;;
        *) printf '' ;;
    esac
}

{
    printf '%s\n' \
        'tokens,compute_mode,timing_mvm_cost_mode,placement_cost_mode,configuration,schedule,greedy_heuristic,lookahead,beam_width,link_pressure,balanced_reductions,reduction_width,random_seed,cpu_issue_width,digital_clock_ghz,digital_vector_bits_per_cycle,memory_backend,status,active_cores,task_count,dependency_count,logical_arrays,inter_core_transfer_bytes,graph_score,simulated_time,simulated_time_ns,output_elements,finite_elements,first_bits,checksum_bits,total_instructions,total_vector_instructions,total_cpu_cycles,total_network_tx_words,total_analog_active_cycles,scheduled_mlir,task_core_map,deployment_route_manifest,simulation_log'

    for token_count in 4 8 16 32; do
        while IFS=$'\t' read -r name schedule heuristic balanced; do
            lookahead=""
            beam_width=""
            link_pressure=0
            if [[ "${heuristic}" =~ lookahead=([0-9]+) ]]; then
                lookahead="${BASH_REMATCH[1]}"
            fi
            if [[ "${heuristic}" =~ beam=([0-9]+) ]]; then
                beam_width="${BASH_REMATCH[1]}"
            fi
            if [[ "${heuristic}" == *link-pressure* ]]; then
                link_pressure=1
            fi
            for mode in analog digital; do
                mode_dir="$(
                    sweep_mode_directory \
                        "${token_count}" \
                        "${name}" \
                        "${mode}"
                )"
                deployment_dir="${mode_dir}/deployment"
                simulation_log="${deployment_dir}/simulation.log"
                scheduler_summary="${mode_dir}/scheduler-summary.csv"
                scheduled_ir="${mode_dir}/10-scheduled.mlir"
                task_core_map="${mode_dir}/task-core-map.csv"
                route_manifest="${mode_dir}/deployment-routes.csv"
                status="not-built"
                active_cores=""
                task_count=""
                dependency_count=""
                logical_arrays=""
                inter_core_bytes=""
                graph_score=""
                placement_cost_mode="n/a"
                if [[ "${schedule}" == "greedy-timing" ]]; then
                    placement_cost_mode="${mode}"
                fi
                simulated_value=""
                simulated_unit=""
                simulated_ns=""
                output_elements=""
                finite_elements=""
                first_bits=""
                checksum_bits=""
                profile_totals=",,,,"

                if [[ -f "${scheduler_summary}" ]]; then
                    read -r \
                        task_count \
                        dependency_count \
                        logical_arrays \
                        inter_core_bytes \
                        graph_score \
                        placement_cost_mode < <(
                        "${COMPILER_PYTHON}" -c '
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    row = next(csv.reader(stream))
print(row[10], row[11], row[12], row[14], row[18], row[26])
' "${scheduler_summary}"
                    )
                fi
                if [[ -f "${mode_dir}/active-cores.txt" ]]; then
                    active_cores="$(wc -l <"${mode_dir}/active-cores.txt")"
                    status="built"
                fi
                if [[ -f "${deployment_dir}/status.failed" ]]; then
                    status="failed"
                elif [[ -f "${deployment_dir}/status.pass" ]]; then
                    status="pass"
                fi

                if [[ -f "${simulation_log}" ]]; then
                    read -r simulated_value simulated_unit < <(
                        awk '
                            /Simulation is complete, simulated time:/ {
                                print $(NF - 1), $NF
                            }
                        ' "${simulation_log}" |
                            tail -n 1
                    ) || true
                    if [[ -n "${simulated_value}" ]]; then
                        simulated_ns="$(
                            simulation_time_ns \
                                "${simulated_value}" \
                                "${simulated_unit}"
                        )"
                    fi

                    read -r \
                        output_elements \
                        finite_elements \
                        first_bits \
                        checksum_bits < <(
                        awk '
                            /GPT2_OUTPUT / {
                                elements = finite = first = checksum = ""
                                for (i = 1; i <= NF; ++i) {
                                    split($i, field, "=")
                                    gsub(/\r/, "", field[2])
                                    if (field[1] == "elements")
                                        elements = field[2]
                                    else if (field[1] == "finite")
                                        finite = field[2]
                                    else if (field[1] == "first_bits")
                                        first = field[2]
                                    else if (field[1] == "checksum_bits")
                                        checksum = field[2]
                                }
                                print elements, finite, first, checksum
                            }
                        ' "${simulation_log}" |
                            tail -n 1
                    ) || true

                    profile_totals="$(
                        awk '
                            /MITTENS_PROFILE / {
                                for (i = 1; i <= NF; ++i) {
                                    split($i, field, "=")
                                    if (field[1] == "instructions")
                                        instructions += field[2]
                                    else if (field[1] == "vector_instructions")
                                        vector_instructions += field[2]
                                    else if (field[1] == "cpu_cycles")
                                        cpu_cycles += field[2]
                                    else if (field[1] == "network_tx_words")
                                        network_tx_words += field[2]
                                    else if (field[1] == "analog_active_cycles")
                                        analog_active_cycles += field[2]
                                }
                                profiles += 1
                            }
                            END {
                                if (profiles == 0)
                                    print ",,,,"
                                else
                                    printf "%.0f,%.0f,%.0f,%.0f,%.0f\n",
                                        instructions,
                                        vector_instructions,
                                        cpu_cycles,
                                        network_tx_words,
                                        analog_active_cycles
                            }
                        ' "${simulation_log}"
                    )"
                fi

                printf '%s,%s,%s,%s,' \
                    "${token_count}" \
                    "${mode}" \
                    "${mode}" \
                    "${placement_cost_mode}"
                csv_string "${name}"
                printf ','
                csv_string "${schedule}"
                printf ','
                csv_string "${heuristic}"
                printf ',%s,%s,%s,%s,%s,%s,%s,%s,%s,' \
                    "${lookahead}" \
                    "${beam_width}" \
                    "${link_pressure}" \
                    "${balanced}" \
                    "${SWEEP_REDUCTION_WIDTH}" \
                    "${SWEEP_RANDOM_SEED}" \
                    "${SWEEP_CPU_ISSUE_WIDTH}" \
                    "${SWEEP_DIGITAL_CLOCK_GHZ}" \
                    "${SWEEP_DIGITAL_VECTOR_BITS_PER_CYCLE}"
                csv_string "${SWEEP_MEMORY_BACKEND}"
                printf ','
                csv_string "${status}"
                printf ',%s,%s,%s,%s,%s,%s,' \
                    "${active_cores}" \
                    "${task_count}" \
                    "${dependency_count}" \
                    "${logical_arrays}" \
                    "${inter_core_bytes}" \
                    "${graph_score}"
                if [[ -n "${simulated_value}" ]]; then
                    csv_string "${simulated_value} ${simulated_unit}"
                else
                    csv_string ""
                fi
                printf ',%s,%s,%s,%s,%s,%s,' \
                    "${simulated_ns}" \
                    "${output_elements}" \
                    "${finite_elements}" \
                    "${first_bits}" \
                    "${checksum_bits}" \
                    "${profile_totals}"
                csv_string "${scheduled_ir}"
                printf ','
                csv_string "${task_core_map}"
                printf ','
                csv_string "${route_manifest}"
                printf ','
                csv_string "${simulation_log}"
                printf '\n'
            done
        done < <(sweep_configuration_rows)
    done
} >"${TEMPORARY_RESULTS}"

mv -- "${TEMPORARY_RESULTS}" "${SWEEP_RESULTS}"

pass_count="$(
    find "${SWEEP_OUTPUT_ROOT}" -type f -name status.pass -print |
        wc -l
)"
failed_count="$(
    find "${SWEEP_OUTPUT_ROOT}" -type f -name status.failed -print |
        wc -l
)"
mode_build_count="$(
    find "${SWEEP_OUTPUT_ROOT}" -type f \
        \( -path '*/analog/.complete' -o -path '*/digital/.complete' \) \
        -print |
        wc -l
)"
built_count="$((mode_build_count - pass_count - failed_count))"
echo "GPT-2 sweep results: ${pass_count} passed, ${built_count} built, ${failed_count} failed, 112 total"
echo "GPT-2 sweep CSV: ${SWEEP_RESULTS}"
