#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=factorial-common.sh
source "${SCRIPT_DIR}/factorial-common.sh"

readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly TORCH_MLIR_OPT="${INSTALL_ROOT}/torch-mlir/bin/torch-mlir-opt"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly CORE_OBJECT_BUILDER="${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"
readonly ROUTE_EXTRACTOR="${PROJECT_ROOT}/scripts/extract-deployment-routes.py"
readonly TASK_MAP_EXTRACTOR="${PROJECT_ROOT}/scripts/extract-task-core-map.py"
readonly CORE_BUILD_JOBS="${MITTENS_GPT2_FACTORIAL_BUILD_JOBS:-${BUILD_JOBS}}"

factorial_validate_settings
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
    echo "MITTENS_GPT2_FACTORIAL_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi

run_opt_stage() {
    local input="$1"
    local output="$2"
    shift 2
    local temporary="${output}.tmp"
    local diagnostics="${output}.log"
    local temporary_diagnostics="${diagnostics}.tmp"
    local warning_count

    rm -f -- "${temporary}" "${temporary_diagnostics}"
    if ! "${SCULPTOR_OPT}" "${input}" "$@" -o "${temporary}" \
        >"${temporary_diagnostics}" 2>&1; then
        mv -- "${temporary_diagnostics}" "${diagnostics}"
        echo "Sculptor stage failed; final diagnostics follow: ${diagnostics}" >&2
        tail -n 120 -- "${diagnostics}" >&2
        return 1
    fi
    mv -- "${temporary_diagnostics}" "${diagnostics}"
    mv -- "${temporary}" "${output}"
    warning_count="$(grep -c ': warning:' "${diagnostics}" || true)"
    if [[ "${warning_count}" -gt 0 ]]; then
        echo "captured ${warning_count} compiler warnings in ${diagnostics}"
    fi
}

build_common_graph() {
    local token_count="$1"
    local common_dir="${FACTORIAL_OUTPUT_ROOT}/tokens-${token_count}/common"
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

    if [[ "${FACTORIAL_FORCE_BUILD}" == 0 && -f "${complete}" ]]; then
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
        "${FACTORIAL_GPT2_TEST_DIR}/gpt2_small.py" \
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
        "--sculptor-expand-mvm-to-golem=array-rows=${FACTORIAL_ANALOG_ARRAY_ROWS} array-cols=${FACTORIAL_ANALOG_ARRAY_COLUMNS}"

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
        printf 'pipeline_version=%s\n' "${FACTORIAL_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'profile=gpt2-small\n'
        printf 'array_rows=%s\n' "${FACTORIAL_ANALOG_ARRAY_ROWS}"
        printf 'array_columns=%s\n' "${FACTORIAL_ANALOG_ARRAY_COLUMNS}"
    } >"${common_dir}/build-metadata.txt"
    touch -- "${complete}"

    if [[ "${FACTORIAL_KEEP_IR}" == 0 ]]; then
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

build_graph_variant() {
    local token_count="$1"
    local balanced_reductions="$2"
    local distributed_matmul="$3"
    local common_graph="${FACTORIAL_OUTPUT_ROOT}/tokens-${token_count}/common/08-task-graph.mlir"
    local variant_dir
    local distributed_ir
    local balanced_ir
    local optimized_ir
    local islands_ir
    local complete
    local current_ir

    variant_dir="$(
        factorial_variant_directory \
            "${token_count}" \
            "${balanced_reductions}" \
            "${distributed_matmul}"
    )"
    distributed_ir="${variant_dir}/08-distributed.mlir"
    balanced_ir="${variant_dir}/08-balanced.mlir"
    optimized_ir="${variant_dir}/08-optimizer-all.mlir"
    islands_ir="${variant_dir}/09-islands.mlir"
    complete="${variant_dir}/.complete"

    if [[ "${FACTORIAL_FORCE_BUILD}" == 0 && -f "${complete}" ]]; then
        require_file "${islands_ir}"
        echo "[tokens ${token_count} / rb${balanced_reductions}-dm${distributed_matmul}] reuse graph variant"
        return
    fi

    echo "[tokens ${token_count} / rb${balanced_reductions}-dm${distributed_matmul}] build graph variant"
    mkdir -p -- "${variant_dir}"
    rm -f -- "${complete}"
    current_ir="${common_graph}"

    run_opt_stage "${current_ir}" "${optimized_ir}" \
        '--sculptor-optimize-task-graph=patterns=all require-change=true'
    current_ir="${optimized_ir}"

    if [[ "${distributed_matmul}" == 1 ]]; then
        run_opt_stage "${current_ir}" "${distributed_ir}" \
            --sculptor-distribute-digital-matmul="strategy=auto max-shards=8 min-ops-per-shard=1 placement-policy=prefer-distinct require-change=true"
        current_ir="${distributed_ir}"
    fi
    if [[ "${balanced_reductions}" == 1 ]]; then
        run_opt_stage "${current_ir}" "${balanced_ir}" \
            --sculptor-balance-task-graph-reductions="reduction-width=${FACTORIAL_REDUCTION_WIDTH} require-change=true"
        current_ir="${balanced_ir}"
    fi
    run_opt_stage "${current_ir}" "${islands_ir}" \
        "--sculptor-build-task-graph-islands=digital-assignment=${FACTORIAL_ISLAND_ASSIGNMENT}"

    if ! grep -q 'sculptor.schedule.island_id' "${islands_ir}"; then
        echo "GPT-2 token-${token_count} graph variant has no islands" >&2
        return 1
    fi

    {
        printf 'pipeline_version=%s\n' "${FACTORIAL_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'balanced_reductions=%s\n' "${balanced_reductions}"
        printf 'distributed_matmul=%s\n' "${distributed_matmul}"
        printf 'reduction_width=%s\n' "${FACTORIAL_REDUCTION_WIDTH}"
        printf 'distribution_strategy=auto\n'
        printf 'distribution_max_shards=8\n'
        printf 'distribution_min_ops_per_shard=1\n'
        printf 'distribution_placement_policy=prefer-distinct\n'
        printf 'island_assignment=%s\n' "${FACTORIAL_ISLAND_ASSIGNMENT}"
    } >"${variant_dir}/variant-metadata.txt"
    touch -- "${complete}"

    if [[ "${FACTORIAL_KEEP_IR}" == 0 ]]; then
        rm -f -- "${optimized_ir}" "${distributed_ir}" "${balanced_ir}"
    fi
}

build_schedule() {
    local token_count="$1"
    local name="$2"
    local boundary_regret="$3"
    local compact_region="$4"
    local link_pressure="$5"
    local balanced_reductions="$6"
    local distributed_matmul="$7"
    local heuristic="$8"
    local variant_dir
    local configuration_dir
    local islands_ir
    local timed_ir
    local scheduled_ir
    local scheduler_summary
    local task_core_map
    local schedule_complete
    local metadata
    local schedule_options

    variant_dir="$(
        factorial_variant_directory \
            "${token_count}" \
            "${balanced_reductions}" \
            "${distributed_matmul}"
    )"
    configuration_dir="$(
        factorial_configuration_directory "${token_count}" "${name}"
    )"
    islands_ir="${variant_dir}/09-islands.mlir"
    timed_ir="${configuration_dir}/09-timed.mlir"
    scheduled_ir="${configuration_dir}/10-scheduled.mlir"
    scheduler_summary="${configuration_dir}/scheduler-summary.csv"
    task_core_map="${configuration_dir}/task-core-map.csv"
    schedule_complete="${configuration_dir}/.schedule-complete"
    metadata="${configuration_dir}/configuration.txt"

    if [[ "${FACTORIAL_FORCE_BUILD}" == 0 &&
          -f "${schedule_complete}" &&
          -f "${scheduled_ir}" &&
          -f "${scheduler_summary}" &&
          -f "${task_core_map}" &&
          -f "${metadata}" ]] &&
       grep -Fxq "pipeline_version=${FACTORIAL_PIPELINE_VERSION}" "${metadata}" &&
       grep -Fxq "greedy_heuristic=${heuristic}" "${metadata}" &&
       grep -Fxq "timing_options=${FACTORIAL_TIMING_OPTIONS}" "${metadata}"; then
        echo "[tokens ${token_count} / ${name}] reuse schedule"
        return
    fi

    echo "[tokens ${token_count} / ${name}] analyze timing and schedule"
    mkdir -p -- "${configuration_dir}"
    rm -f -- \
        "${schedule_complete}" \
        "${configuration_dir}/.complete" \
        "${scheduler_summary}" \
        "${configuration_dir}/deployment/status.pass" \
        "${configuration_dir}/deployment/status.failed"
    run_opt_stage "${islands_ir}" "${timed_ir}" \
        "--sculptor-analyze-task-graph-timing=${FACTORIAL_TIMING_OPTIONS}"

    schedule_options="cores=144 arrays-per-core=${FACTORIAL_ARRAYS_PER_CORE} topology=mesh mesh-rows=${FACTORIAL_MESH_HEIGHT} mesh-cols=${FACTORIAL_MESH_WIDTH} schedule=greedy-timing greedy-heuristic=${heuristic} summary-output=${scheduler_summary}"
    run_opt_stage "${timed_ir}" "${scheduled_ir}" \
        "--sculptor-schedule-task-graph=${schedule_options}"

    if ! grep -q 'sculptor.runtime.core_id' "${scheduled_ir}" ||
       ! grep -q 'sculptor.schedule.placement_cost_mode = "analog"' "${scheduled_ir}" ||
       [[ "$(wc -l <"${scheduler_summary}")" -ne 1 ]]; then
        echo "GPT-2 token-${token_count} ${name} schedule is incomplete" >&2
        return 1
    fi
    "${TASK_MAP_EXTRACTOR}" "${scheduled_ir}" "${task_core_map}"

    {
        printf 'pipeline_version=%s\n' "${FACTORIAL_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'configuration=%s\n' "${name}"
        printf 'schedule=greedy-timing\n'
        printf 'greedy_heuristic=%s\n' "${heuristic}"
        printf 'boundary_regret=%s\n' "${boundary_regret}"
        printf 'compact_region=%s\n' "${compact_region}"
        printf 'spatial_link_pressure=%s\n' "${link_pressure}"
        printf 'balanced_reductions=%s\n' "${balanced_reductions}"
        printf 'distributed_matmul=%s\n' "${distributed_matmul}"
        printf 'timing_mvm_cost_mode=analog\n'
        printf 'placement_cost_mode=analog\n'
        printf 'digital_clock_ghz=%s\n' "${FACTORIAL_DIGITAL_CLOCK_GHZ}"
        printf 'digital_issue_width=%s\n' "${FACTORIAL_CPU_ISSUE_WIDTH}"
        printf 'digital_vector_bits_per_cycle=%s\n' \
            "${FACTORIAL_DIGITAL_VECTOR_BITS_PER_CYCLE}"
        printf 'timing_options=%s\n' "${FACTORIAL_TIMING_OPTIONS}"
    } >"${metadata}"
    touch -- "${schedule_complete}"

    if [[ "${FACTORIAL_KEEP_IR}" == 0 ]]; then
        rm -f -- "${timed_ir}"
    fi
}

build_configuration() {
    local token_count="$1"
    local name="$2"
    local configuration_dir
    local scheduled_ir
    local fused_ir
    local optimized_ir
    local partitioned_ir
    local compiler_timing_model
    local route_manifest
    local complete
    local pass_marker
    local core_build_log
    local temporary_core_build_log
    local core_count
    local core_id

    configuration_dir="$(
        factorial_configuration_directory "${token_count}" "${name}"
    )"
    scheduled_ir="${configuration_dir}/10-scheduled.mlir"
    fused_ir="${configuration_dir}/11-fused.mlir"
    optimized_ir="${configuration_dir}/12-optimizer-all.mlir"
    partitioned_ir="${configuration_dir}/partitioned.mlir"
    compiler_timing_model="${configuration_dir}/compiler-timing-model.json"
    route_manifest="${configuration_dir}/deployment-routes.csv"
    complete="${configuration_dir}/.complete"
    pass_marker="${configuration_dir}/deployment/status.pass"

    if [[ "${FACTORIAL_FORCE_BUILD}" == 0 &&
          -f "${pass_marker}" &&
          -f "${compiler_timing_model}" ]]; then
        require_file "${configuration_dir}/scheduler-summary.csv"
        require_file "${configuration_dir}/task-core-map.csv"
        require_file "${route_manifest}"
        echo "[tokens ${token_count} / ${name}] simulation already passed"
        return
    fi
    if [[ "${FACTORIAL_FORCE_BUILD}" == 0 &&
          -f "${complete}" &&
          -f "${compiler_timing_model}" ]]; then
        require_file "${configuration_dir}/active-cores.txt"
        while read -r core_id; do
            require_file "${configuration_dir}/cores/core-${core_id}.o"
        done <"${configuration_dir}/active-cores.txt"
        echo "[tokens ${token_count} / ${name}] reuse core objects"
        return
    fi

    echo "[tokens ${token_count} / ${name}] fuse, lower, and partition"
    rm -f -- "${complete}"
    run_opt_stage "${scheduled_ir}" "${fused_ir}" \
        --sculptor-fuse-task-graph
    run_opt_stage "${fused_ir}" "${optimized_ir}" \
        '--sculptor-optimize-task-graph=patterns=all require-change=true'
    run_opt_stage "${optimized_ir}" "${partitioned_ir}" \
        "--sculptor-analyze-task-graph-timing=${FACTORIAL_TIMING_OPTIONS}" \
        "--sculptor-export-task-graph-sim-model=output=${compiler_timing_model}" \
        --sculptor-lower-golem-to-llvm-shims \
        --sculptor-partition-task-graph-by-core
    require_file "${compiler_timing_model}"
    "${ROUTE_EXTRACTOR}" \
        --mesh-width "${FACTORIAL_MESH_WIDTH}" \
        "${partitioned_ir}" \
        "${route_manifest}"

    core_count="$(
        rg -o '^  module @core_[0-9]+' "${partitioned_ir}" |
            sort -u |
            wc -l
    )"
    if [[ "${core_count}" -lt 1 || "${core_count}" -gt 144 ]]; then
        echo "expected 1 to 144 active cores, found ${core_count}" >&2
        return 1
    fi
    if ! grep -q 'golem_analog_mvm_compute' "${partitioned_ir}"; then
        echo "analog lowering contains no Golem MVM calls" >&2
        return 1
    fi

    echo "[tokens ${token_count} / ${name}] build ${core_count} core objects"
    core_build_log="${configuration_dir}/core-object-build.log"
    temporary_core_build_log="${core_build_log}.tmp"
    rm -f -- "${temporary_core_build_log}"
    if ! SCULPTOR_PARTITIONED_MLIR="${partitioned_ir}" \
         SCULPTOR_CORE_OBJECT_DIR="${configuration_dir}/cores" \
         SCULPTOR_ACTIVE_CORE_MANIFEST="${configuration_dir}/active-cores.txt" \
         SCULPTOR_CORE_REGALLOC_FALLBACK_MANIFEST="${configuration_dir}/regalloc-fallback-cores.txt" \
         SCULPTOR_CORE_BUILD_JOBS="${CORE_BUILD_JOBS}" \
         SCULPTOR_CORE_LTO=none \
         SCULPTOR_CORE_REGALLOC="${FACTORIAL_CORE_REGALLOC}" \
         SCULPTOR_CORE_REGALLOC_FALLBACK=none \
         SCULPTOR_CORE_REUSE_OBJECTS=1 \
            "${CORE_OBJECT_BUILDER}" \
            >"${temporary_core_build_log}" 2>&1; then
        mv -- "${temporary_core_build_log}" "${core_build_log}"
        echo "core-object build failed; final diagnostics follow: ${core_build_log}" >&2
        tail -n 120 -- "${core_build_log}" >&2
        return 1
    fi
    mv -- "${temporary_core_build_log}" "${core_build_log}"
    echo "built ${core_count} core objects; log: ${core_build_log}"

    {
        printf 'pipeline_version=%s\n' "${FACTORIAL_PIPELINE_VERSION}"
        printf 'tokens=%s\n' "${token_count}"
        printf 'configuration=%s\n' "${name}"
        printf 'compute_mode=analog\n'
        printf 'active_cores=%s\n' "${core_count}"
        printf 'register_allocator=%s\n' "${FACTORIAL_CORE_REGALLOC}"
        printf 'register_allocator_fallback=none\n'
        printf 'register_allocator_fallback_core_count=%s\n' \
            "$(wc -l <"${configuration_dir}/regalloc-fallback-cores.txt")"
    } >"${configuration_dir}/build-metadata.txt"
    touch -- "${complete}"

    if [[ "${FACTORIAL_KEEP_IR}" == 0 ]]; then
        find "${configuration_dir}/cores" -maxdepth 1 -type f \
            ! -name '*.o' -delete
        rm -f -- "${partitioned_ir}" "${optimized_ir}" "${scheduled_ir}"
    fi
}

configuration_build_is_reusable() {
    local token_count="$1"
    local name="$2"
    local configuration_dir
    local core_id

    configuration_dir="$(
        factorial_configuration_directory "${token_count}" "${name}"
    )"
    if [[ -f "${configuration_dir}/deployment/status.pass" &&
          -f "${configuration_dir}/compiler-timing-model.json" ]]; then
        return 0
    fi
    if [[ ! -f "${configuration_dir}/.complete" ||
          ! -f "${configuration_dir}/active-cores.txt" ||
          ! -f "${configuration_dir}/compiler-timing-model.json" ]]; then
        return 1
    fi
    while read -r core_id; do
        if [[ ! -f "${configuration_dir}/cores/core-${core_id}.o" ]]; then
            return 1
        fi
    done <"${configuration_dir}/active-cores.txt"
    return 0
}

readonly BUILD_PROGRESS_TOTAL="$(factorial_selected_count)"
readonly BUILD_PROGRESS_STARTED_AT="${SECONDS}"
BUILD_PROGRESS_ORDINAL=0

for token_count in ${FACTORIAL_TOKEN_SELECTION}; do
    build_common_graph "${token_count}"
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
        BUILD_PROGRESS_ORDINAL=$((BUILD_PROGRESS_ORDINAL + 1))
        completed="$(factorial_count_selected_marker '.complete')"
        factorial_print_progress \
            BUILD \
            "${BUILD_PROGRESS_ORDINAL}" \
            "${BUILD_PROGRESS_TOTAL}" \
            "${token_count}" \
            "${name}" \
            "${completed}" \
            0 \
            "${BUILD_PROGRESS_STARTED_AT}"
        if [[ "${FACTORIAL_FORCE_BUILD}" == 0 ]] &&
           configuration_build_is_reusable "${token_count}" "${name}"; then
            echo "[tokens ${token_count} / ${name}] reuse completed deployment"
            continue
        fi
        build_graph_variant \
            "${token_count}" \
            "${balanced_reductions}" \
            "${distributed_matmul}"
        build_schedule \
            "${token_count}" \
            "${name}" \
            "${boundary_regret}" \
            "${compact_region}" \
            "${link_pressure}" \
            "${balanced_reductions}" \
            "${distributed_matmul}" \
            "${heuristic}"
        build_configuration "${token_count}" "${name}"
    done < <(factorial_configuration_rows)
done

"${SCRIPT_DIR}/summarize-results.sh"
completed="$(factorial_count_selected_marker '.complete')"
factorial_print_final_progress \
    BUILD \
    "${BUILD_PROGRESS_TOTAL}" \
    "${completed}" \
    0 \
    "${BUILD_PROGRESS_STARTED_AT}"
echo "GPT-2 factorial build matrix: COMPLETE"
