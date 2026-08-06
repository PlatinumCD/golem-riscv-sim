#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sweep-common.sh
source "${SCRIPT_DIR}/sweep-common.sh"

readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly TORCH_MLIR_OPT="${INSTALL_ROOT}/torch-mlir/bin/torch-mlir-opt"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly CORE_OBJECT_BUILDER="${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"
readonly ROUTE_EXTRACTOR="${PROJECT_ROOT}/scripts/extract-deployment-routes.py"
readonly TASK_MAP_EXTRACTOR="${PROJECT_ROOT}/scripts/extract-task-core-map.py"
readonly CORE_BUILD_JOBS="${MITTENS_GPT2_SWEEP_BUILD_JOBS:-${BUILD_JOBS}}"

sweep_validate_settings
for executable in \
    "${COMPILER_PYTHON}" \
    "${TORCH_MLIR_OPT}" \
    "${SCULPTOR_OPT}" \
    "${CORE_OBJECT_BUILDER}" \
    "${ROUTE_EXTRACTOR}" \
    "${TASK_MAP_EXTRACTOR}"; do
    require_executable "${executable}"
done
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"
if [[ ! "${CORE_BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_SWEEP_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi

run_opt_stage() {
    local input="$1"
    local output="$2"
    shift 2
    local temporary="${output}.tmp"

    rm -f -- "${temporary}"
    "${SCULPTOR_OPT}" "${input}" "$@" -o "${temporary}"
    mv -- "${temporary}" "${output}"
}

build_common_graph() {
    local token_count="$1"
    local common_dir="${SWEEP_OUTPUT_ROOT}/tokens-${token_count}/common"
    local complete="${common_dir}/.complete"
    local torch_ir="${common_dir}/01-torch.mlir"
    local linalg_ir="${common_dir}/02-linalg.mlir"
    local canonical_ir="${common_dir}/03-canonical.mlir"
    local extracted_ir="${common_dir}/04-extracted.mlir"
    local converted_ir="${common_dir}/05-converted.mlir"
    local golem_ir="${common_dir}/06-golem.mlir"
    local tasks_ir="${common_dir}/07-tasks.mlir"
    local task_graph_ir="${common_dir}/08-task-graph.mlir"
    local temporary

    if [[ "${SWEEP_FORCE_BUILD}" == 0 && -f "${complete}" ]]; then
        require_file "${task_graph_ir}"
        echo "[tokens ${token_count}] reuse common task graph"
        return
    fi

    echo "[tokens ${token_count}] export GPT-2-small Torch IR"
    mkdir -p -- "${common_dir}"
    rm -f -- "${complete}"
    temporary="${torch_ir}.tmp"
    rm -f -- "${temporary}"
    PYTHONPATH="${TORCH_MLIR_PYTHON}" \
        "${COMPILER_PYTHON}" \
        "${SWEEP_GPT2_TEST_DIR}/gpt2_small.py" \
        --mode mlir \
        --profile gpt2-small \
        --seq-len "${token_count}" \
        >"${temporary}"
    mv -- "${temporary}" "${torch_ir}"

    echo "[tokens ${token_count}] Torch-MLIR to Linalg"
    temporary="${linalg_ir}.tmp"
    rm -f -- "${temporary}"
    "${TORCH_MLIR_OPT}" "${torch_ir}" \
        --torch-backend-to-linalg-on-tensors-backend-pipeline \
        -o "${temporary}"
    mv -- "${temporary}" "${linalg_ir}"

    echo "[tokens ${token_count}] canonicalize and extract layers"
    run_opt_stage "${linalg_ir}" "${canonical_ir}" \
        --sculptor-canonicalize-layers
    run_opt_stage "${canonical_ir}" "${extracted_ir}" \
        --sculptor-extract-layers

    echo "[tokens ${token_count}] convert and tile MVMs"
    run_opt_stage "${extracted_ir}" "${converted_ir}" \
        --sculptor-convert-layers
    run_opt_stage "${converted_ir}" "${golem_ir}" \
        "--sculptor-expand-mvm-to-golem=array-rows=1024 array-cols=512"

    echo "[tokens ${token_count}] materialize and assemble task graph"
    run_opt_stage "${golem_ir}" "${tasks_ir}" \
        --sculptor-materialize-tasks
    run_opt_stage "${tasks_ir}" "${task_graph_ir}" \
        --sculptor-assemble-task-graph

    if ! grep -q 'sculptor.task_graph.create' "${task_graph_ir}" ||
       ! grep -q 'sculptor.task.create' "${task_graph_ir}"; then
        echo "GPT-2 token-${token_count} task graph is empty" >&2
        return 1
    fi

    {
        printf 'tokens=%s\n' "${token_count}"
        printf 'profile=gpt2-small\n'
        printf 'array_rows=1024\n'
        printf 'array_columns=512\n'
    } >"${common_dir}/build-metadata.txt"
    touch -- "${complete}"

    if [[ "${SWEEP_KEEP_IR}" == 0 ]]; then
        rm -f -- \
            "${torch_ir}" \
            "${linalg_ir}" \
            "${canonical_ir}" \
            "${extracted_ir}" \
            "${converted_ir}" \
            "${golem_ir}" \
            "${tasks_ir}"
    fi
}

build_placement_input() {
    local token_count="$1"
    local name="$2"
    local balanced="$3"
    local common_dir="${SWEEP_OUTPUT_ROOT}/tokens-${token_count}/common"
    local task_graph_ir="${common_dir}/08-task-graph.mlir"
    local configuration_dir
    local balanced_ir
    local islands_ir
    local placement_complete

    configuration_dir="$(
        sweep_configuration_directory "${token_count}" "${name}"
    )"
    balanced_ir="${configuration_dir}/08-balanced.mlir"
    islands_ir="${configuration_dir}/09-islands.mlir"
    placement_complete="${configuration_dir}/.placement-complete"

    if [[ "${SWEEP_FORCE_BUILD}" == 0 &&
          -f "${placement_complete}" ]]; then
        require_file "${islands_ir}"
        echo "[tokens ${token_count} / ${name}] reuse placement input"
        return
    fi

    echo "[tokens ${token_count} / ${name}] build placement input"
    mkdir -p -- "${configuration_dir}"
    rm -f -- "${placement_complete}"
    if [[ "${balanced}" == 1 ]]; then
        run_opt_stage "${task_graph_ir}" "${balanced_ir}" \
            "--sculptor-balance-task-graph-reductions=reduction-width=${SWEEP_REDUCTION_WIDTH} require-change=true"
    else
        cp -- "${task_graph_ir}" "${balanced_ir}"
    fi
    run_opt_stage "${balanced_ir}" "${islands_ir}" \
        --sculptor-build-task-graph-islands

    {
        printf 'tokens=%s\n' "${token_count}"
        printf 'configuration=%s\n' "${name}"
        printf 'balanced_reductions=%s\n' "${balanced}"
        printf 'reduction_width=%s\n' "${SWEEP_REDUCTION_WIDTH}"
    } >"${configuration_dir}/configuration.txt"
    touch -- "${placement_complete}"

    if [[ "${SWEEP_KEEP_IR}" == 0 ]]; then
        rm -f -- "${balanced_ir}"
    fi
}

build_schedule() {
    local token_count="$1"
    local name="$2"
    local mode="$3"
    local schedule="$4"
    local heuristic="$5"
    local configuration_dir
    local mode_dir
    local islands_ir
    local timed_ir
    local scheduled_ir
    local scheduler_summary
    local task_core_map
    local schedule_complete
    local scheduling_metadata
    local schedule_options
    local timing_options
    local placement_cost_mode

    configuration_dir="$(
        sweep_configuration_directory "${token_count}" "${name}"
    )"
    mode_dir="$(sweep_mode_directory "${token_count}" "${name}" "${mode}")"
    islands_ir="${configuration_dir}/09-islands.mlir"
    timed_ir="${mode_dir}/09-timed.mlir"
    scheduled_ir="${mode_dir}/10-scheduled.mlir"
    scheduler_summary="${mode_dir}/scheduler-summary.csv"
    task_core_map="${mode_dir}/task-core-map.csv"
    schedule_complete="${mode_dir}/.schedule-complete"
    scheduling_metadata="${mode_dir}/scheduling-metadata.txt"
    timing_options="$(sweep_timing_options_for_mode "${mode}")"
    placement_cost_mode="n/a"
    if [[ "${schedule}" == "greedy-timing" ]]; then
        placement_cost_mode="${mode}"
    fi

    if [[ "${SWEEP_FORCE_BUILD}" == 0 &&
          -f "${schedule_complete}" &&
          -f "${task_core_map}" &&
          -f "${scheduling_metadata}" ]] &&
       grep -Fxq \
           "pipeline_version=${SWEEP_PIPELINE_VERSION}" \
           "${scheduling_metadata}" &&
       grep -Fxq \
           "timing_options=${timing_options}" \
           "${scheduling_metadata}" &&
       head -n 1 "${task_core_map}" |
           grep -q 'timing_mvm_cost_mode'; then
        require_file "${scheduled_ir}"
        require_file "${scheduler_summary}"
        require_file "${task_core_map}"
        if [[ "${placement_cost_mode}" == "n/a" ]]; then
            if grep -q 'sculptor.schedule.placement_cost_mode' \
                "${scheduled_ir}"; then
                echo "cached ${mode} schedule unexpectedly has a placement cost mode" >&2
                return 1
            fi
        elif ! grep -q \
            "sculptor.schedule.placement_cost_mode = \"${placement_cost_mode}\"" \
            "${scheduled_ir}"; then
            echo "cached ${mode} schedule has the wrong placement cost mode" >&2
            return 1
        fi
        echo "[tokens ${token_count} / ${name} / ${mode}] reuse schedule"
        return
    fi

    echo "[tokens ${token_count} / ${name} / ${mode}] timing and schedule=${schedule}"
    mkdir -p -- "${mode_dir}"
    rm -f -- \
        "${schedule_complete}" \
        "${scheduler_summary}" \
        "${mode_dir}/.complete" \
        "${mode_dir}/deployment/status.pass" \
        "${mode_dir}/deployment/status.failed"
    if [[ -d "${mode_dir}/cores" ]]; then
        find "${mode_dir}/cores" -maxdepth 1 -type f \
            -name 'core-*.o' -delete
    fi
    run_opt_stage "${islands_ir}" "${timed_ir}" \
        "--sculptor-analyze-task-graph-timing=${timing_options}"

    schedule_options="cores=64 arrays-per-core=4 topology=mesh mesh-rows=8 mesh-cols=8 schedule=${schedule} random-seed=${SWEEP_RANDOM_SEED} summary-output=${scheduler_summary}"
    if [[ "${heuristic}" != "none" ]]; then
        schedule_options+=" greedy-heuristic=${heuristic}"
    fi
    run_opt_stage "${timed_ir}" "${scheduled_ir}" \
        "--sculptor-schedule-task-graph=${schedule_options}"

    if ! grep -q 'sculptor.runtime.core_id' "${scheduled_ir}" ||
       ! grep -q \
           "sculptor.timing.mvm_cost_mode = \"${mode}\"" \
           "${scheduled_ir}" ||
       [[ "$(wc -l <"${scheduler_summary}")" -ne 1 ]]; then
        echo "GPT-2 token-${token_count} ${name} ${mode} schedule is incomplete" >&2
        return 1
    fi
    if [[ "${placement_cost_mode}" == "n/a" ]]; then
        if grep -q 'sculptor.schedule.placement_cost_mode' "${scheduled_ir}"; then
            echo "GPT-2 token-${token_count} ${name} should not consume placement timing costs" >&2
            return 1
        fi
    elif ! grep -q \
        "sculptor.schedule.placement_cost_mode = \"${placement_cost_mode}\"" \
        "${scheduled_ir}"; then
        echo "GPT-2 token-${token_count} ${name} did not preserve ${placement_cost_mode} placement costs" >&2
        return 1
    fi
    "${TASK_MAP_EXTRACTOR}" "${scheduled_ir}" "${task_core_map}"

    {
        printf 'pipeline_version=%s\n' "${SWEEP_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'configuration=%s\n' "${name}"
        printf 'schedule=%s\n' "${schedule}"
        printf 'greedy_heuristic=%s\n' "${heuristic}"
        printf 'timing_mvm_cost_mode=%s\n' "${mode}"
        printf 'placement_cost_mode=%s\n' "${placement_cost_mode}"
        printf 'execution_backend=%s\n' "${mode}"
        printf 'random_seed=%s\n' "${SWEEP_RANDOM_SEED}"
        printf 'digital_clock_ghz=%s\n' "${SWEEP_DIGITAL_CLOCK_GHZ}"
        printf 'digital_issue_width=%s\n' "${SWEEP_CPU_ISSUE_WIDTH}"
        printf 'digital_vector_bits_per_cycle=%s\n' \
            "${SWEEP_DIGITAL_VECTOR_BITS_PER_CYCLE}"
        printf 'timing_options=%s\n' "${timing_options}"
    } >"${scheduling_metadata}"
    touch -- "${schedule_complete}"

    if [[ "${SWEEP_KEEP_IR}" == 0 ]]; then
        rm -f -- "${timed_ir}"
    fi
}

build_mode() {
    local token_count="$1"
    local name="$2"
    local mode="$3"
    local configuration_dir
    local mode_dir
    local scheduled_ir
    local partitioned_ir
    local route_manifest
    local mode_complete
    local scheduling_metadata
    local placement_cost_mode
    local core_count
    local core_id
    local -a lowering_passes

    configuration_dir="$(
        sweep_configuration_directory "${token_count}" "${name}"
    )"
    mode_dir="$(sweep_mode_directory "${token_count}" "${name}" "${mode}")"
    scheduled_ir="${mode_dir}/10-scheduled.mlir"
    partitioned_ir="${mode_dir}/partitioned.mlir"
    route_manifest="${mode_dir}/deployment-routes.csv"
    mode_complete="${mode_dir}/.complete"
    scheduling_metadata="${mode_dir}/scheduling-metadata.txt"
    require_file "${scheduling_metadata}"
    placement_cost_mode="$(
        awk -F= '
            $1 == "placement_cost_mode" {
                print $2
                found = 1
                exit
            }
            END {
                if (!found)
                    exit 1
            }
        ' "${scheduling_metadata}"
    )"

    if [[ "${SWEEP_FORCE_BUILD}" == 0 && -f "${mode_complete}" ]]; then
        require_file "${mode_dir}/active-cores.txt"
        while read -r core_id; do
            require_file "${mode_dir}/cores/core-${core_id}.o"
        done <"${mode_dir}/active-cores.txt"
        echo "[tokens ${token_count} / ${name} / ${mode}] reuse core objects"
        return
    fi

    echo "[tokens ${token_count} / ${name} / ${mode}] lower and partition"
    mkdir -p -- "${mode_dir}"
    rm -f -- "${mode_complete}"
    lowering_passes=()
    if [[ "${mode}" == "digital" ]]; then
        lowering_passes+=(--sculptor-lower-scheduled-mvm-to-digital)
    fi
    lowering_passes+=(
        --sculptor-fuse-task-graph
        "--sculptor-analyze-task-graph-timing=$(sweep_timing_options_for_mode "${mode}")"
        --sculptor-lower-golem-to-llvm-shims
        --sculptor-partition-task-graph-by-core
    )
    run_opt_stage \
        "${scheduled_ir}" \
        "${partitioned_ir}" \
        "${lowering_passes[@]}"
    "${ROUTE_EXTRACTOR}" \
        --mesh-width 8 \
        "${partitioned_ir}" \
        "${route_manifest}"

    core_count="$(
        rg -o '^  module @core_[0-9]+' "${partitioned_ir}" |
            sort -u |
            wc -l
    )"
    if [[ "${core_count}" -lt 1 || "${core_count}" -gt 64 ]]; then
        echo "expected 1 to 64 active cores, found ${core_count}" >&2
        return 1
    fi
    if [[ "${mode}" == "analog" ]]; then
        if ! grep -q 'golem_analog_mvm_compute' "${partitioned_ir}"; then
            echo "analog lowering contains no Golem MVM calls" >&2
            return 1
        fi
    elif grep -Eq \
        'sculptor\.array\.|golem_analog_mvm_(set|load|compute|store)' \
        "${partitioned_ir}" ||
        ! grep -q 'linalg.matmul_transpose_b' "${partitioned_ir}"; then
        echo "digital lowering retains analog operations or has no matmul" >&2
        return 1
    fi

    echo "[tokens ${token_count} / ${name} / ${mode}] build ${core_count} objects"
    SCULPTOR_PARTITIONED_MLIR="${partitioned_ir}" \
    SCULPTOR_CORE_OBJECT_DIR="${mode_dir}/cores" \
    SCULPTOR_ACTIVE_CORE_MANIFEST="${mode_dir}/active-cores.txt" \
    SCULPTOR_CORE_REGALLOC_FALLBACK_MANIFEST="${mode_dir}/regalloc-fallback-cores.txt" \
    SCULPTOR_CORE_BUILD_JOBS="${CORE_BUILD_JOBS}" \
    SCULPTOR_CORE_LTO=none \
    SCULPTOR_CORE_REGALLOC="${SWEEP_CORE_REGALLOC}" \
    SCULPTOR_CORE_REGALLOC_FALLBACK=none \
    SCULPTOR_CORE_REUSE_OBJECTS=1 \
        "${CORE_OBJECT_BUILDER}"

    {
        printf 'pipeline_version=%s\n' "${SWEEP_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'configuration=%s\n' "${name}"
        printf 'compute_mode=%s\n' "${mode}"
        printf 'timing_mvm_cost_mode=%s\n' "${mode}"
        printf 'placement_cost_mode=%s\n' "${placement_cost_mode}"
        printf 'active_cores=%s\n' "${core_count}"
        printf 'register_allocator=%s\n' "${SWEEP_CORE_REGALLOC}"
        printf 'register_allocator_fallback=none\n'
        printf 'register_allocator_fallback_core_count=%s\n' \
            "$(wc -l <"${mode_dir}/regalloc-fallback-cores.txt")"
    } >"${mode_dir}/build-metadata.txt"
    touch -- "${mode_complete}"

    if [[ "${SWEEP_KEEP_IR}" == 0 ]]; then
        find "${mode_dir}/cores" -maxdepth 1 -type f \
            ! -name '*.o' -delete
        rm -f -- "${partitioned_ir}"
    fi
}

readonly BUILD_PROGRESS_TOTAL="$(sweep_selected_deployment_count)"
readonly BUILD_PROGRESS_STARTED_AT="${SECONDS}"
BUILD_PROGRESS_ORDINAL=0

for token_count in ${SWEEP_TOKEN_SELECTION}; do
    build_common_graph "${token_count}"
    while IFS=$'\t' read -r name schedule heuristic balanced; do
        if ! sweep_item_selected \
            "${name}" \
            "${SWEEP_CONFIGURATION_SELECTION}"; then
            continue
        fi
        build_placement_input \
            "${token_count}" \
            "${name}" \
            "${balanced}"
        for mode in ${SWEEP_MODE_SELECTION}; do
            build_schedule \
                "${token_count}" \
                "${name}" \
                "${mode}" \
                "${schedule}" \
                "${heuristic}"
            BUILD_PROGRESS_ORDINAL=$((BUILD_PROGRESS_ORDINAL + 1))
            if [[ "${SWEEP_FORCE_BUILD}" == 1 ]]; then
                build_completed=$((BUILD_PROGRESS_ORDINAL - 1))
            else
                build_completed="$(
                    sweep_count_selected_mode_marker '.complete'
                )"
            fi
            sweep_print_progress \
                BUILD \
                "${BUILD_PROGRESS_ORDINAL}" \
                "${BUILD_PROGRESS_TOTAL}" \
                "${token_count}" \
                "${name}" \
                "${mode}" \
                "${build_completed}" \
                0 \
                "${BUILD_PROGRESS_STARTED_AT}" \
                Complete
            build_mode "${token_count}" "${name}" "${mode}"
        done
    done < <(sweep_configuration_rows)
done

"${SCRIPT_DIR}/summarize-results.sh"
if [[ "${SWEEP_FORCE_BUILD}" == 1 ]]; then
    build_completed="${BUILD_PROGRESS_TOTAL}"
else
    build_completed="$(sweep_count_selected_mode_marker '.complete')"
fi
sweep_print_final_progress \
    BUILD \
    "${BUILD_PROGRESS_TOTAL}" \
    "${build_completed}" \
    0 \
    "${BUILD_PROGRESS_STARTED_AT}" \
    Complete
echo "GPT-2 scheduling sweep build matrix: COMPLETE"
