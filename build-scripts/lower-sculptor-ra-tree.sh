#!/usr/bin/env bash
set -euo pipefail

# Lower one tensor-level MLIR module through Sculptor's RA-tree deployment
# boundary.  By default the output contains one placed deployment module.  A
# model-suite caller may instead request one-pass outlining and extraction so
# large deployments never have to serialize and parse a multi-gigabyte global
# tile module.

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly INPUT="${SCULPTOR_INPUT_MLIR:?SCULPTOR_INPUT_MLIR must name tensor-level input MLIR}"
readonly OUTPUT_DIR="${SCULPTOR_DEPLOYMENT_DIR:?SCULPTOR_DEPLOYMENT_DIR must name an output directory}"
readonly OPT="${SCULPTOR_OPT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt}"
readonly SPLIT="${SCULPTOR_SPLIT_TILE_DEPLOYMENT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-split-tile-deployment}"
readonly PRE_SPLIT_CORE_DIR="${SCULPTOR_PRE_SPLIT_CORE_DIR:-}"
readonly PRE_SPLIT_ACTIVE_CORE_MANIFEST="${SCULPTOR_PRE_SPLIT_ACTIVE_CORE_MANIFEST:-}"
readonly PRE_SPLIT_DEPLOYMENT_MANIFEST="${SCULPTOR_PRE_SPLIT_DEPLOYMENT_MANIFEST:-}"
readonly FIXED_SHARD_BYTES="${SCULPTOR_FIXED_SHARD_BYTES:-4096}"
readonly STREAMING_SCRATCHPAD_BYTES="${SCULPTOR_STREAMING_SCRATCHPAD_BYTES:-2097152}"
readonly GLOBAL_RAM_BYTES="${SCULPTOR_GLOBAL_RAM_BYTES:-34359738368}"
readonly MAX_IN_FLIGHT="${SCULPTOR_MAX_IN_FLIGHT:-2}"
readonly MESH_ROWS="${SCULPTOR_MESH_ROWS:-1}"
readonly MESH_COLS="${SCULPTOR_MESH_COLS:-1}"
readonly ARRAYS_PER_CORE="${SCULPTOR_ARRAYS_PER_CORE:-1}"
readonly ARRAY_ROWS="${SCULPTOR_ARRAY_ROWS:-8}"
readonly ARRAY_COLS="${SCULPTOR_ARRAY_COLS:-8}"
readonly SEQUENCE_SHARD_ROWS="${SCULPTOR_SEQUENCE_SHARD_ROWS:-0}"
readonly SEQUENCE_SHARD_BYTES="${SCULPTOR_SEQUENCE_SHARD_BYTES:-67108864}"
readonly FUSE_POINTWISE_EPILOGUES="${SCULPTOR_FUSE_POINTWISE_EPILOGUES:-1}"
readonly SEQUENCE_WAVES_IN_FLIGHT="${SCULPTOR_SEQUENCE_WAVES_IN_FLIGHT:-1}"
readonly DIGITAL_WORKERS="${SCULPTOR_DIGITAL_WORKERS:-1}"
readonly REQUESTED_MVM_SEQUENCE_WORKERS="${SCULPTOR_MVM_SEQUENCE_WORKERS:-0}"
readonly PROPORTIONAL_MVM_SEQUENCING="${SCULPTOR_PROPORTIONAL_MVM_SEQUENCING:-0}"
readonly DIGITAL_MINIMUM_WORK_ITEMS_PER_UNIT="${SCULPTOR_DIGITAL_MINIMUM_WORK_ITEMS_PER_UNIT:-4096}"
readonly COST_AWARE_EXPANSION="${SCULPTOR_COST_AWARE_EXPANSION:-false}"
readonly WORK_UNIT_DISPATCH_CYCLES="${SCULPTOR_WORK_UNIT_DISPATCH_CYCLES:-0}"
readonly MINIMUM_EXPANSION_SPEEDUP_PERCENT="${SCULPTOR_MINIMUM_EXPANSION_SPEEDUP_PERCENT:-110}"
readonly REQUIRE_CHANGE="${SCULPTOR_REQUIRE_CHANGE:-false}"
readonly DIGITAL_DATAFLOW="${SCULPTOR_DATAFLOW:-sharded}"
readonly DIGITAL_TILING_POLICY="${SCULPTOR_DIGITAL_TILING_POLICY:-communication-aware}"
readonly SHARD_PROPAGATION_DEPTH="${SCULPTOR_SHARD_PROPAGATION_DEPTH:-0}"
readonly REQUIRE_COMPLETE_SHARD_CHAIN="${SCULPTOR_REQUIRE_COMPLETE_SHARD_CHAIN:-false}"
readonly REDUCTION_TREE="${SCULPTOR_REDUCTION_TREE:-none}"
readonly REDUCTION_FAN_IN="${SCULPTOR_REDUCTION_FAN_IN:-2}"
readonly REDUCTION_MINIMUM_WIDTH="${SCULPTOR_REDUCTION_MINIMUM_WIDTH:-3}"
readonly DUPLICATE_MATRICES="${SCULPTOR_DUPLICATE_MATRICES:-0}"
readonly MATRIX_MINIMUM_MVMS_PER_REPLICA="${SCULPTOR_MATRIX_MINIMUM_MVMS_PER_REPLICA:-8}"
readonly MATRIX_MAXIMUM_REPLICAS_PER_SETUP="${SCULPTOR_MATRIX_MAXIMUM_REPLICAS_PER_SETUP:-0}"
readonly BALANCE_DIGITAL_WORK="${SCULPTOR_BALANCE_DIGITAL_WORK:-0}"
readonly DIGITAL_SCHEDULING_POLICY="${SCULPTOR_DIGITAL_SCHEDULING_POLICY:-}"
readonly DIGITAL_WINDOW_SIZE="${SCULPTOR_DIGITAL_WINDOW_SIZE:-0}"
readonly MVM_BODY_POLICY="${SCULPTOR_MVM_BODY_POLICY:-spread}"
readonly PLANNER_STRATEGIES="${SCULPTOR_PLANNER_STRATEGIES:-setup-first,layer-cut,recursive-fork-join}"
readonly SETUP_BINDING_POLICY="${SCULPTOR_SETUP_BINDING_POLICY:-consumer-anchored}"
readonly MAPPING_OBJECTIVE="${SCULPTOR_OBJECTIVE:-latency}"
readonly COST_PROFILE="${SCULPTOR_COST_PROFILE:-}"
readonly LOCAL_MEMORY_BYTES_PER_CORE="${SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE:-${STREAMING_SCRATCHPAD_BYTES}}"
readonly CLOCK_FREQUENCY_HZ="${SCULPTOR_CLOCK_FREQUENCY_HZ:-1000000000}"
readonly ANALOG_MVM_LATENCY_NS="${SCULPTOR_ANALOG_MVM_LATENCY_NS:-100}"
readonly ANALOG_IO_BITS_PER_CYCLE="${SCULPTOR_ANALOG_IO_BITS_PER_CYCLE:-256}"
readonly ANALOG_IO_POLICY="${SCULPTOR_ANALOG_IO_POLICY:-shared}"
readonly ANALOG_ARRAY_EXECUTION="${SCULPTOR_ANALOG_ARRAY_EXECUTION:-concurrent}"
readonly DIGITAL_ISSUE_WIDTH="${SCULPTOR_DIGITAL_ISSUE_WIDTH:-2}"
readonly DIGITAL_VECTOR_BITS_PER_CYCLE="${SCULPTOR_DIGITAL_VECTOR_BITS_PER_CYCLE:-256}"
readonly NETWORK_WORD_BITS="${SCULPTOR_NETWORK_WORD_BITS:-32}"
readonly NETWORK_HOP_CYCLES="${SCULPTOR_NETWORK_HOP_CYCLES:-1}"
readonly NETWORK_CONTENTION_MODEL="${SCULPTOR_NETWORK_CONTENTION_MODEL:-link-serialized}"
readonly VERIFY_PLAN="${SCULPTOR_VERIFY_PLAN:-true}"
readonly PLACEMENT_SCHEDULE="${SCULPTOR_PLACEMENT_SCHEDULE:-greedy}"
readonly PLACEMENT_OBJECTIVE="${SCULPTOR_PLACEMENT_OBJECTIVE:-transfer-cost}"
readonly TEMPORAL_NETWORK_MODE="${SCULPTOR_TEMPORAL_NETWORK_MODE:-finite}"
readonly TIMING_SCOPE="${SCULPTOR_TIMING_SCOPE:-warm}"
readonly TEMPORAL_CANDIDATE_LIMIT="${SCULPTOR_TEMPORAL_CANDIDATE_LIMIT:-8}"
readonly GREEDY_TILE_ORDER="${SCULPTOR_GREEDY_TILE_ORDER:-sequential}"
readonly GREEDY_PRIORITY_MODE="${SCULPTOR_GREEDY_PRIORITY_MODE:-sum}"
readonly GREEDY_CANDIDATE_SCOPE="${SCULPTOR_GREEDY_CANDIDATE_SCOPE:-cardinal}"
readonly GREEDY_LOOKAHEAD="${SCULPTOR_GREEDY_LOOKAHEAD:-1}"
readonly RANDOM_SEED="${SCULPTOR_RANDOM_SEED:-0}"
readonly VERIFY_PLACEMENT="${SCULPTOR_VERIFY_PLACEMENT:-true}"
readonly TILE_MEMORY_CAPACITY_BYTES="${SCULPTOR_TILE_MEMORY_CAPACITY_BYTES:-${STREAMING_SCRATCHPAD_BYTES}}"
readonly FUSE_PRODUCER_CONSUMER="${SCULPTOR_FUSE_PRODUCER_CONSUMER:-0}"
readonly CONSOLIDATE_LAYER_REGIONS="${SCULPTOR_CONSOLIDATE_LAYER_REGIONS:-0}"
readonly SPECIALIZE_CONV_PATCH_GENERATION="${SCULPTOR_SPECIALIZE_CONV_PATCH_GENERATION:-1}"
readonly RETAIN_PROVED_LOCAL_OWNERS="${SCULPTOR_RETAIN_PROVED_LOCAL_OWNERS:-1}"
readonly RETAINED_OWNER_BOUNDARY_IDS="${SCULPTOR_RETAINED_OWNER_BOUNDARY_IDS:-}"
readonly EXACT_RAM_READINESS="${SCULPTOR_EXACT_RAM_READINESS:-1}"
readonly CONTRACT_SCALAR_EXECUTION_REGIONS="${SCULPTOR_CONTRACT_SCALAR_EXECUTION_REGIONS:-1}"
readonly REQUESTED_EXECUTION_RESIDENCY_MODE="${SCULPTOR_EXECUTION_RESIDENCY_REGIONS:-analyze}"
readonly EXECUTION_RESIDENCY_MAXIMUM_MEMBERS="${SCULPTOR_EXECUTION_RESIDENCY_MAXIMUM_MEMBERS:-8}"
readonly EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH="${SCULPTOR_EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH:-2}"
readonly REQUESTED_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT="${SCULPTOR_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT:-true}"
readonly EXECUTION_RESIDENCY_AUDIT_OUTPUT="${SCULPTOR_EXECUTION_RESIDENCY_AUDIT_OUTPUT:-${OUTPUT_DIR}/execution-residency-audit.json}"
readonly COMPILER_STAGE_TIMEOUT_SECONDS="${SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS:-300}"
readonly STOP_AFTER_STAGE="${SCULPTOR_STOP_AFTER_STAGE:-}"

require_file "${INPUT}"
require_executable "${OPT}"
require_command timeout
require_command cut

if [[ -n "${PRE_SPLIT_CORE_DIR}" ||
      -n "${PRE_SPLIT_ACTIVE_CORE_MANIFEST}" ||
      -n "${PRE_SPLIT_DEPLOYMENT_MANIFEST}" ]]; then
    if [[ -z "${PRE_SPLIT_CORE_DIR}" ||
          -z "${PRE_SPLIT_ACTIVE_CORE_MANIFEST}" ||
          -z "${PRE_SPLIT_DEPLOYMENT_MANIFEST}" ]]; then
        echo "SCULPTOR_PRE_SPLIT_CORE_DIR, SCULPTOR_PRE_SPLIT_ACTIVE_CORE_MANIFEST, and SCULPTOR_PRE_SPLIT_DEPLOYMENT_MANIFEST must be set together" >&2
        exit 2
    fi
    require_executable "${SPLIT}"
fi

if [[ ! "${COMPILER_STAGE_TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS must be a positive integer" >&2
    exit 2
fi
case "${STOP_AFTER_STAGE}" in
    ""|06-mapping-plan|08-placed) ;;
    *)
        echo "SCULPTOR_STOP_AFTER_STAGE must be 06-mapping-plan, 08-placed, or empty" >&2
        exit 2
        ;;
esac

case "${FIXED_SHARD_BYTES}" in
    4096|8192|16384|32768|65536|131072|262144) ;;
    *)
        echo "SCULPTOR_FIXED_SHARD_BYTES must be one of 4096, 8192, 16384, 32768, 65536, 131072, or 262144" >&2
        exit 2
        ;;
esac
if [[ ! "${STREAMING_SCRATCHPAD_BYTES}" =~ ^[1-9][0-9]*$ ]] ||
   ((STREAMING_SCRATCHPAD_BYTES < FIXED_SHARD_BYTES ||
      STREAMING_SCRATCHPAD_BYTES > 16 * 1024 * 1024)); then
    echo "SCULPTOR_STREAMING_SCRATCHPAD_BYTES must be at least the configured frame maximum and at most 16 MiB" >&2
    exit 2
fi
if [[ ! "${GLOBAL_RAM_BYTES}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_GLOBAL_RAM_BYTES must be a positive integer" >&2
    exit 2
fi
if [[ ! "${MAX_IN_FLIGHT}" =~ ^[1-8]$ ]]; then
    echo "SCULPTOR_MAX_IN_FLIGHT must be between 1 and 8" >&2
    exit 2
fi

for value in "${MESH_ROWS}" "${MESH_COLS}" "${ARRAYS_PER_CORE}" \
    "${ARRAY_ROWS}" "${ARRAY_COLS}" "${DIGITAL_WORKERS}" \
    "${DIGITAL_MINIMUM_WORK_ITEMS_PER_UNIT}"; do
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "RA-tree hardware values must be positive integers" >&2
        exit 2
    fi
done
readonly MATRIX_ARRAY_CAPACITY=$((MESH_ROWS * MESH_COLS * ARRAYS_PER_CORE))
if [[ "${DUPLICATE_MATRICES}" != 0 && "${DUPLICATE_MATRICES}" != 1 ]]; then
    echo "SCULPTOR_DUPLICATE_MATRICES must be 0 or 1" >&2
    exit 2
fi
if [[ "${PROPORTIONAL_MVM_SEQUENCING}" != 0 &&
      "${PROPORTIONAL_MVM_SEQUENCING}" != 1 ]]; then
    echo "SCULPTOR_PROPORTIONAL_MVM_SEQUENCING must be 0 or 1" >&2
    exit 2
fi
if [[ "${PROPORTIONAL_MVM_SEQUENCING}" == 1 &&
      "${DUPLICATE_MATRICES}" != 1 ]]; then
    echo "proportional MVM sequencing requires matrix duplication" >&2
    exit 2
fi

# MVM sequence workers and general digital workers are independent resources.
# Zero retains the historical automatic policy for existing callers.  Without
# matrix replication, retain the one-range representation so several sequence
# workers do not contend for the same analog lane and add a join for no
# parallel benefit.
if [[ ! "${REQUESTED_MVM_SEQUENCE_WORKERS}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_MVM_SEQUENCE_WORKERS must be a nonnegative integer" >&2
    exit 2
fi
MVM_SEQUENCE_WORKERS=1
if [[ "${DUPLICATE_MATRICES}" == 1 ]]; then
    if [[ "${REQUESTED_MVM_SEQUENCE_WORKERS}" == 0 ]]; then
        MVM_SEQUENCE_WORKERS="${DIGITAL_WORKERS}"
    else
        MVM_SEQUENCE_WORKERS="${REQUESTED_MVM_SEQUENCE_WORKERS}"
    fi
elif ((REQUESTED_MVM_SEQUENCE_WORKERS > 1)); then
    echo "SCULPTOR_MVM_SEQUENCE_WORKERS greater than one requires matrix replication" >&2
    exit 2
fi
readonly MVM_SEQUENCE_WORKERS
if [[ ! "${MATRIX_MINIMUM_MVMS_PER_REPLICA}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_MATRIX_MINIMUM_MVMS_PER_REPLICA must be a positive integer" >&2
    exit 2
fi
if [[ ! "${MATRIX_MAXIMUM_REPLICAS_PER_SETUP}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_MATRIX_MAXIMUM_REPLICAS_PER_SETUP must be a nonnegative integer" >&2
    exit 2
fi
if ((MATRIX_MAXIMUM_REPLICAS_PER_SETUP > 0 &&
     MVM_SEQUENCE_WORKERS > MATRIX_MAXIMUM_REPLICAS_PER_SETUP)); then
    echo "MVM sequence workers cannot exceed the matrix replica cap" >&2
    exit 2
fi
if [[ "${BALANCE_DIGITAL_WORK}" != 0 && "${BALANCE_DIGITAL_WORK}" != 1 ]]; then
    echo "SCULPTOR_BALANCE_DIGITAL_WORK must be 0 or 1" >&2
    exit 2
fi
for value in "${REQUIRE_CHANGE}" "${REQUIRE_COMPLETE_SHARD_CHAIN}" \
    "${COST_AWARE_EXPANSION}" \
    "${VERIFY_PLAN}" "${VERIFY_PLACEMENT}"; do
    if [[ "${value}" != true && "${value}" != false && \
          "${value}" != 1 && "${value}" != 0 ]]; then
        echo "Sculptor boolean controls must be true, false, 1, or 0" >&2
        exit 2
    fi
done
if [[ ! "${WORK_UNIT_DISPATCH_CYCLES}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_WORK_UNIT_DISPATCH_CYCLES must be a nonnegative integer" >&2
    exit 2
fi
if [[ ! "${MINIMUM_EXPANSION_SPEEDUP_PERCENT}" =~ ^[1-9][0-9]*$ ]] ||
   ((MINIMUM_EXPANSION_SPEEDUP_PERCENT < 100)); then
    echo "SCULPTOR_MINIMUM_EXPANSION_SPEEDUP_PERCENT must be at least 100" >&2
    exit 2
fi
if [[ ! "${DIGITAL_ISSUE_WIDTH}" =~ ^[1-9][0-9]*$ ]] ||
   [[ ! "${DIGITAL_VECTOR_BITS_PER_CYCLE}" =~ ^[1-9][0-9]*$ ]] ||
   ((DIGITAL_VECTOR_BITS_PER_CYCLE % 32 != 0)) ||
   [[ ! "${NETWORK_WORD_BITS}" =~ ^[1-9][0-9]*$ ]] ||
   ((NETWORK_WORD_BITS % 8 != 0)); then
    echo "Sculptor digital throughput and network width must be positive whole-word values" >&2
    exit 2
fi
if [[ -n "${DIGITAL_SCHEDULING_POLICY}" &&
      "${DIGITAL_SCHEDULING_POLICY}" != affinity &&
      "${DIGITAL_SCHEDULING_POLICY}" != balanced &&
      "${DIGITAL_SCHEDULING_POLICY}" != earliest-finish &&
      "${DIGITAL_SCHEDULING_POLICY}" != progressive &&
      "${DIGITAL_SCHEDULING_POLICY}" != sliding-window ]]; then
    echo "SCULPTOR_DIGITAL_SCHEDULING_POLICY must be affinity, balanced, earliest-finish, progressive, sliding-window, or empty" >&2
    exit 2
fi
if [[ ! "${DIGITAL_WINDOW_SIZE}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_DIGITAL_WINDOW_SIZE must be a nonnegative integer" >&2
    exit 2
fi
if [[ "${MVM_BODY_POLICY}" != packed &&
      "${MVM_BODY_POLICY}" != spread &&
      "${MVM_BODY_POLICY}" != first-use-window &&
      "${MVM_BODY_POLICY}" != first-use-adaptive ]]; then
    echo "SCULPTOR_MVM_BODY_POLICY must be packed, spread, first-use-window, or first-use-adaptive" >&2
    exit 2
fi
if [[ ( "${MVM_BODY_POLICY}" == first-use-window ||
        "${MVM_BODY_POLICY}" == first-use-adaptive ) &&
      "${DIGITAL_SCHEDULING_POLICY}" != sliding-window ]]; then
    echo "SCULPTOR_MVM_BODY_POLICY=${MVM_BODY_POLICY} requires SCULPTOR_DIGITAL_SCHEDULING_POLICY=sliding-window" >&2
    exit 2
fi
if [[ "${DIGITAL_SCHEDULING_POLICY}" == sliding-window &&
      "${DIGITAL_WINDOW_SIZE}" == 0 ]]; then
    echo "SCULPTOR_DIGITAL_WINDOW_SIZE must be positive for sliding-window" >&2
    exit 2
fi
if [[ ! "${TILE_MEMORY_CAPACITY_BYTES}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_TILE_MEMORY_CAPACITY_BYTES must be a nonnegative integer" >&2
    exit 2
fi
if [[ ! "${GREEDY_LOOKAHEAD}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_GREEDY_LOOKAHEAD must be a positive integer" >&2
    exit 2
fi
if [[ ! "${SEQUENCE_SHARD_ROWS}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_SEQUENCE_SHARD_ROWS must be a nonnegative integer" >&2
    exit 2
fi
if [[ ! "${SEQUENCE_SHARD_BYTES}" =~ ^[0-9]+$ ]] ||
   ((SEQUENCE_SHARD_ROWS > 0 && SEQUENCE_SHARD_BYTES > 0)); then
    echo "SCULPTOR sequence shard rows/bytes must be nonnegative and mutually exclusive" >&2
    exit 2
fi
if [[ "${FUSE_POINTWISE_EPILOGUES}" != 0 &&
      "${FUSE_POINTWISE_EPILOGUES}" != 1 ]]; then
    echo "SCULPTOR_FUSE_POINTWISE_EPILOGUES must be 0 or 1" >&2
    exit 2
fi
if [[ ! "${SEQUENCE_WAVES_IN_FLIGHT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_SEQUENCE_WAVES_IN_FLIGHT must be a positive integer" >&2
    exit 2
fi
if [[ "${FUSE_PRODUCER_CONSUMER}" != 0 &&
      "${FUSE_PRODUCER_CONSUMER}" != 1 ]]; then
    echo "SCULPTOR_FUSE_PRODUCER_CONSUMER must be 0 or 1" >&2
    exit 2
fi
if [[ "${CONSOLIDATE_LAYER_REGIONS}" != 0 &&
      "${CONSOLIDATE_LAYER_REGIONS}" != 1 ]]; then
    echo "SCULPTOR_CONSOLIDATE_LAYER_REGIONS must be 0 or 1" >&2
    exit 2
fi
if [[ "${SPECIALIZE_CONV_PATCH_GENERATION}" != 0 &&
      "${SPECIALIZE_CONV_PATCH_GENERATION}" != 1 ]]; then
    echo "SCULPTOR_SPECIALIZE_CONV_PATCH_GENERATION must be 0 or 1" >&2
    exit 2
fi
if [[ "${RETAIN_PROVED_LOCAL_OWNERS}" != 0 &&
      "${RETAIN_PROVED_LOCAL_OWNERS}" != 1 ]]; then
    echo "SCULPTOR_RETAIN_PROVED_LOCAL_OWNERS must be 0 or 1" >&2
    exit 2
fi
if [[ "${EXACT_RAM_READINESS}" != 0 &&
      "${EXACT_RAM_READINESS}" != 1 ]]; then
    echo "SCULPTOR_EXACT_RAM_READINESS must be 0 or 1" >&2
    exit 2
fi
if [[ "${CONTRACT_SCALAR_EXECUTION_REGIONS}" != 0 &&
      "${CONTRACT_SCALAR_EXECUTION_REGIONS}" != 1 ]]; then
    echo "SCULPTOR_CONTRACT_SCALAR_EXECUTION_REGIONS must be 0 or 1" >&2
    exit 2
fi
case "${REQUESTED_EXECUTION_RESIDENCY_MODE}" in
    0|off) EXECUTION_RESIDENCY_MODE=off ;;
    analyze) EXECUTION_RESIDENCY_MODE=analyze ;;
    1|select) EXECUTION_RESIDENCY_MODE=select ;;
    *)
        echo "SCULPTOR_EXECUTION_RESIDENCY_REGIONS must be off, analyze, select, 0, or 1" >&2
        exit 2
        ;;
esac
readonly EXECUTION_RESIDENCY_MODE
if [[ ! "${EXECUTION_RESIDENCY_MAXIMUM_MEMBERS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_EXECUTION_RESIDENCY_MAXIMUM_MEMBERS must be a positive integer" >&2
    exit 2
fi
if [[ ! "${EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH}" =~ ^[1-8]$ ]]; then
    echo "SCULPTOR_EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH must be between 1 and 8" >&2
    exit 2
fi
case "${REQUESTED_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT}" in
    1|true) EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT=true ;;
    0|false) EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT=false ;;
    *)
        echo "SCULPTOR_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT must be true, false, 1, or 0" >&2
        exit 2
        ;;
esac
readonly EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT
if [[ -n "${RETAINED_OWNER_BOUNDARY_IDS}" &&
      ! "${RETAINED_OWNER_BOUNDARY_IDS}" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
    echo "SCULPTOR_RETAINED_OWNER_BOUNDARY_IDS must be empty or a comma-separated list of nonnegative integers" >&2
    exit 2
fi
if [[ -n "${RETAINED_OWNER_BOUNDARY_IDS}" &&
      "${RETAIN_PROVED_LOCAL_OWNERS}" != 1 ]]; then
    echo "SCULPTOR_RETAINED_OWNER_BOUNDARY_IDS requires SCULPTOR_RETAIN_PROVED_LOCAL_OWNERS=1" >&2
    exit 2
fi
case "${DIGITAL_DATAFLOW}" in
    bulk|sharded|adaptive) ;;
    *) echo "SCULPTOR_DATAFLOW must be bulk, sharded, or adaptive" >&2; exit 2 ;;
esac
case "${DIGITAL_TILING_POLICY}" in
    dimension-first|communication-aware) ;;
    *) echo "SCULPTOR_DIGITAL_TILING_POLICY is invalid" >&2; exit 2 ;;
esac
case "${SETUP_BINDING_POLICY}" in
    global|consumer-anchored) ;;
    *) echo "SCULPTOR_SETUP_BINDING_POLICY must be global or consumer-anchored" >&2; exit 2 ;;
esac
if [[ ! "${SHARD_PROPAGATION_DEPTH}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_SHARD_PROPAGATION_DEPTH must be nonnegative" >&2
    exit 2
fi
case "${REDUCTION_TREE}" in
    none|balanced) ;;
    *) echo "SCULPTOR_REDUCTION_TREE must be none or balanced" >&2; exit 2 ;;
esac
if [[ "${REDUCTION_FAN_IN}" != 2 ]]; then
    echo "SCULPTOR_REDUCTION_FAN_IN must be 2" >&2
    exit 2
fi
if [[ ! "${REDUCTION_MINIMUM_WIDTH}" =~ ^[2-9][0-9]*$ ]]; then
    echo "SCULPTOR_REDUCTION_MINIMUM_WIDTH must be an integer of at least 2" >&2
    exit 2
fi

mkdir -p -- "${OUTPUT_DIR}"
readonly STAGE_METRICS="${OUTPUT_DIR}/compiler-stage-metrics.csv"
readonly STAGE_DIAGNOSTICS_DIR="${OUTPUT_DIR}/diagnostics"
mkdir -p -- "${STAGE_DIAGNOSTICS_DIR}"
printf 'stage,wall_seconds,max_rss_kb,output_bytes,operation_count,exit_code,diagnostics,crash_reproducer\n' \
    >"${STAGE_METRICS}"

run_stage() {
    local input="$1"
    local output="$2"
    local stage diagnostics crash_reproducer time_file exit_code
    local wall_seconds max_rss_kb output_bytes operation_count
    local from_checkpoint through_checkpoint pass_options="" option
    shift 2
    from_checkpoint="${input##*/}"
    if [[ "${input}" == "${INPUT}" ]]; then
        from_checkpoint=input
    fi
    through_checkpoint="${output##*/}"
    for option in "$@"; do
        pass_options+="${pass_options:+;}${option}"
    done
    stage="$(basename -- "${output}" .mlir)"
    diagnostics="${STAGE_DIAGNOSTICS_DIR}/${stage}.log"
    crash_reproducer="${STAGE_DIAGNOSTICS_DIR}/${stage}-crash-reproducer.mlir"
    time_file="${STAGE_DIAGNOSTICS_DIR}/${stage}-resource-usage.csv"
    rm -f -- "${output}" "${diagnostics}" "${crash_reproducer}" "${time_file}"

    set +e
    /usr/bin/time -q -f '%e,%M' -o "${time_file}" \
        timeout --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${OPT}" "${input}" --verify-each \
        --mlir-print-op-on-diagnostic=false \
        --mlir-timing --mlir-timing-display=tree --print-op-stats \
        "--mlir-pass-pipeline-crash-reproducer=${crash_reproducer}" \
        "--sculptor-pipeline=profile=parent from=${from_checkpoint} through=${through_checkpoint} pass-options={${pass_options}}" \
        -o "${output}" 2>&1 | cut -c 1-8192 >"${diagnostics}"
    exit_code="${PIPESTATUS[0]}"
    set -e

    wall_seconds=NA
    max_rss_kb=NA
    if [[ -s "${time_file}" ]]; then
        IFS=, read -r wall_seconds max_rss_kb <"${time_file}"
    fi
    output_bytes=0
    if [[ -f "${output}" ]]; then
        output_bytes="$(stat -c '%s' -- "${output}")"
    fi
    operation_count="$(awk -F',' '
        /^  [^,]+ , [0-9]+$/ {
            value = $2
            gsub(/[^0-9]/, "", value)
            total += value
            found = 1
        }
        END { print found ? total : "NA" }
    ' "${diagnostics}")"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${stage}" "${wall_seconds}" "${max_rss_kb}" "${output_bytes}" \
        "${operation_count}" "${exit_code}" "${diagnostics}" \
        "${crash_reproducer}" >>"${STAGE_METRICS}"
    if [[ "${exit_code}" != 0 ]]; then
        if [[ "${exit_code}" == 124 ]]; then
            echo "compiler stage ${stage} exceeded the ${COMPILER_STAGE_TIMEOUT_SECONDS}-second cutoff" >&2
        fi
        cat -- "${diagnostics}" >&2
        return "${exit_code}"
    fi
    printf '[compiler-stage] %s: %.3fs, %s KiB RSS, %s bytes, %s operations\n' \
        "${stage}" "${wall_seconds}" "${max_rss_kb}" "${output_bytes}" \
        "${operation_count}"
}

run_outline_split_stage() {
    local input="$1"
    local diagnostics time_file exit_code wall_seconds max_rss_kb
    local output_bytes=0 operation_count=0 core_id extracted
    local -a outline_options=(
        --outline-placed-deployment
        "--outline-sequence-waves-in-flight=${SEQUENCE_WAVES_IN_FLIGHT}"
        "--outline-specialize-conv-patch-generation=${SPECIALIZE_CONV_PATCH_GENERATION}"
        "--outline-contract-scalar-execution-regions=${CONTRACT_SCALAR_EXECUTION_REGIONS}"
        --outline-debug-phase-timing
    )
    if [[ "${FUSE_PRODUCER_CONSUMER}" == 1 ]]; then
        outline_options+=(--outline-fuse-producer-consumer)
    fi
    if [[ "${CONSOLIDATE_LAYER_REGIONS}" == 1 ]]; then
        outline_options+=(--outline-consolidate-layer-regions)
    fi
    if [[ "${RETAIN_PROVED_LOCAL_OWNERS}" == 1 ]]; then
        outline_options+=(--outline-retain-proved-local-owners)
    fi
    if [[ -n "${RETAINED_OWNER_BOUNDARY_IDS}" ]]; then
        outline_options+=("--outline-retained-owner-boundary-ids=${RETAINED_OWNER_BOUNDARY_IDS}")
    fi
    if [[ "${EXACT_RAM_READINESS}" == 1 ]]; then
        outline_options+=(--outline-exact-ram-readiness)
    fi

    diagnostics="${STAGE_DIAGNOSTICS_DIR}/09-tile-deployment-split.log"
    time_file="${STAGE_DIAGNOSTICS_DIR}/09-tile-deployment-split-resource-usage.csv"
    mkdir -p -- "${PRE_SPLIT_CORE_DIR}" \
        "$(dirname -- "${PRE_SPLIT_ACTIVE_CORE_MANIFEST}")" \
        "$(dirname -- "${PRE_SPLIT_DEPLOYMENT_MANIFEST}")"
    rm -f -- "${PRE_SPLIT_ACTIVE_CORE_MANIFEST}" \
        "${PRE_SPLIT_DEPLOYMENT_MANIFEST}" "${diagnostics}" "${time_file}"

    set +e
    /usr/bin/time -q -f '%e,%M' -o "${time_file}" \
        timeout --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${SPLIT}" "${input}" \
        "${outline_options[@]}" \
        "--output-directory=${PRE_SPLIT_CORE_DIR}" \
        "--manifest=${PRE_SPLIT_ACTIVE_CORE_MANIFEST}" \
        "--deployment-manifest=${PRE_SPLIT_DEPLOYMENT_MANIFEST}" \
        2>"${diagnostics}"
    exit_code="$?"
    set -e

    wall_seconds=NA
    max_rss_kb=NA
    if [[ -s "${time_file}" ]]; then
        IFS=, read -r wall_seconds max_rss_kb <"${time_file}"
    fi
    if [[ "${exit_code}" == 0 &&
          ! -s "${PRE_SPLIT_ACTIVE_CORE_MANIFEST}" ]]; then
        echo "one-pass outline/split emitted no active tiles" >&2
        exit_code=1
    fi
    if [[ "${exit_code}" == 0 &&
          ! -s "${PRE_SPLIT_DEPLOYMENT_MANIFEST}" ]]; then
        echo "one-pass outline/split emitted no deployment manifest" >&2
        exit_code=1
    fi
    if [[ "${exit_code}" == 0 ]]; then
        while IFS= read -r core_id; do
            if [[ ! "${core_id}" =~ ^[0-9]+$ ]]; then
                echo "one-pass outline/split emitted invalid tile ID: ${core_id}" >&2
                exit_code=1
                break
            fi
            extracted="${PRE_SPLIT_CORE_DIR}/core-${core_id}-extracted.mlir"
            if [[ ! -s "${extracted}" ]]; then
                echo "one-pass outline/split omitted tile ${core_id}" >&2
                exit_code=1
                break
            fi
            output_bytes=$((output_bytes + $(stat -c '%s' -- "${extracted}")))
            operation_count=$((operation_count + 1))
        done <"${PRE_SPLIT_ACTIVE_CORE_MANIFEST}"
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "09-tile-deployment-split" "${wall_seconds}" "${max_rss_kb}" \
        "${output_bytes}" "${operation_count}" "${exit_code}" \
        "${diagnostics}" "NA" >>"${STAGE_METRICS}"
    if [[ "${exit_code}" != 0 ]]; then
        if [[ "${exit_code}" == 124 ]]; then
            echo "compiler stage 09-tile-deployment-split exceeded the ${COMPILER_STAGE_TIMEOUT_SECONDS}-second cutoff" >&2
        fi
        echo "one-pass outline/split failed; diagnostics: ${diagnostics}" >&2
        return "${exit_code}"
    fi
    printf '[compiler-stage] 09-tile-deployment-split: %.3fs, %s KiB RSS, %s bytes, %s tiles\n' \
        "${wall_seconds}" "${max_rss_kb}" "${output_bytes}" \
        "${operation_count}"
}

run_stage "${INPUT}" "${OUTPUT_DIR}/01-canonical.mlir" \
    "sculptor-configure-streaming-architecture{fixed-shard-bytes=${FIXED_SHARD_BYTES} scratchpad-bytes=${STREAMING_SCRATCHPAD_BYTES} global-ram-bytes=${GLOBAL_RAM_BYTES} max-in-flight=${MAX_IN_FLIGHT}}"
run_stage "${OUTPUT_DIR}/01-canonical.mlir" "${OUTPUT_DIR}/02-converted.mlir"
run_stage "${OUTPUT_DIR}/02-converted.mlir" "${OUTPUT_DIR}/03-layouts.mlir"
run_stage "${OUTPUT_DIR}/03-layouts.mlir" "${OUTPUT_DIR}/03-golem.mlir" \
    "sculptor-expand-mvm-to-golem{array-rows=${ARRAY_ROWS} array-cols=${ARRAY_COLS} sequence-shard-rows=${SEQUENCE_SHARD_ROWS} sequence-shard-bytes=${SEQUENCE_SHARD_BYTES} sequence-workers=${MVM_SEQUENCE_WORKERS} array-capacity=${MATRIX_ARRAY_CAPACITY} proportional-mvm-sequencing=${PROPORTIONAL_MVM_SEQUENCING} fuse-pointwise-epilogues=${FUSE_POINTWISE_EPILOGUES}}"
golem_input="${OUTPUT_DIR}/03-golem.mlir"
if [[ "${DUPLICATE_MATRICES}" == 1 ]]; then
    run_stage "${golem_input}" "${OUTPUT_DIR}/03-duplicate-matrices.mlir" \
        "sculptor-duplicate-matrices{array-capacity=${MATRIX_ARRAY_CAPACITY} minimum-mvms-per-replica=${MATRIX_MINIMUM_MVMS_PER_REPLICA} maximum-replicas-per-setup=${MATRIX_MAXIMUM_REPLICAS_PER_SETUP}}"
    golem_input="${OUTPUT_DIR}/03-duplicate-matrices.mlir"
fi
run_stage "${golem_input}" "${OUTPUT_DIR}/03-resolved-layouts.mlir"
golem_input="${OUTPUT_DIR}/03-resolved-layouts.mlir"
digital_work_items_per_cycle=$((DIGITAL_VECTOR_BITS_PER_CYCLE / 32))
if ((DIGITAL_ISSUE_WIDTH > digital_work_items_per_cycle)); then
    digital_work_items_per_cycle="${DIGITAL_ISSUE_WIDTH}"
fi
network_bytes_per_cycle=$((NETWORK_WORD_BITS / 8))
digital_work_options="parallel-workers=${DIGITAL_WORKERS} minimum-work-items-per-unit=${DIGITAL_MINIMUM_WORK_ITEMS_PER_UNIT} digital-work-items-per-cycle=${digital_work_items_per_cycle} network-bytes-per-cycle=${network_bytes_per_cycle} work-unit-dispatch-cycles=${WORK_UNIT_DISPATCH_CYCLES} minimum-expansion-speedup-percent=${MINIMUM_EXPANSION_SPEEDUP_PERCENT} maximum-concurrent-workers=$((MESH_ROWS * MESH_COLS)) local-memory-bytes-per-worker=${LOCAL_MEMORY_BYTES_PER_CORE} adaptive-dataflow-capacity-bytes=${LOCAL_MEMORY_BYTES_PER_CORE} dataflow=${DIGITAL_DATAFLOW} tiling-policy=${DIGITAL_TILING_POLICY} shard-propagation-depth=${SHARD_PROPAGATION_DEPTH} reduction-tree=${REDUCTION_TREE} reduction-fan-in=${REDUCTION_FAN_IN} reduction-min-width=${REDUCTION_MINIMUM_WIDTH}"
if [[ "${COST_AWARE_EXPANSION}" == true || \
      "${COST_AWARE_EXPANSION}" == 1 ]]; then
    digital_work_options+=" cost-aware-expansion"
fi
if [[ "${REQUIRE_CHANGE}" == true || "${REQUIRE_CHANGE}" == 1 ]]; then
    digital_work_options+=" require-change"
fi
if [[ "${REQUIRE_COMPLETE_SHARD_CHAIN}" == true || \
      "${REQUIRE_COMPLETE_SHARD_CHAIN}" == 1 ]]; then
    digital_work_options+=" require-complete-shard-chain"
fi
run_stage "${golem_input}" "${OUTPUT_DIR}/04-expanded-digital-work.mlir" \
    "sculptor-expand-digital-work{${digital_work_options}}"
run_stage "${OUTPUT_DIR}/04-expanded-digital-work.mlir" \
    "${OUTPUT_DIR}/04-parametric-work.mlir"
run_stage "${OUTPUT_DIR}/04-parametric-work.mlir" \
    "${OUTPUT_DIR}/04-tensor-fragments.mlir"
run_stage "${OUTPUT_DIR}/04-tensor-fragments.mlir" \
    "${OUTPUT_DIR}/04-residency-regions.mlir" \
    "sculptor-plan-residency-regions{mode=${EXECUTION_RESIDENCY_MODE} maximum-members=${EXECUTION_RESIDENCY_MAXIMUM_MEMBERS} maximum-wave-width=${EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH} require-positive-benefit=${EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT} audit-output=\"${EXECUTION_RESIDENCY_AUDIT_OUTPUT}\" source-input=\"${OUTPUT_DIR}/04-tensor-fragments.mlir\"}"
run_stage "${OUTPUT_DIR}/04-residency-regions.mlir" "${OUTPUT_DIR}/05-ra-tree.mlir"
mapping_options="strategies=${PLANNER_STRATEGIES} objective=${MAPPING_OBJECTIVE} mvm-body-policy=${MVM_BODY_POLICY} setup-binding-policy=${SETUP_BINDING_POLICY} mesh-rows=${MESH_ROWS} mesh-cols=${MESH_COLS} arrays-per-core=${ARRAYS_PER_CORE} array-rows=${ARRAY_ROWS} array-cols=${ARRAY_COLS} local-memory-bytes-per-core=${LOCAL_MEMORY_BYTES_PER_CORE} clock-frequency-hz=${CLOCK_FREQUENCY_HZ} analog-mvm-latency-ns=${ANALOG_MVM_LATENCY_NS} analog-io-bits-per-cycle=${ANALOG_IO_BITS_PER_CYCLE} analog-io-policy=${ANALOG_IO_POLICY} analog-array-execution=${ANALOG_ARRAY_EXECUTION} digital-issue-width=${DIGITAL_ISSUE_WIDTH} digital-vector-bits-per-cycle=${DIGITAL_VECTOR_BITS_PER_CYCLE} network-word-bits=${NETWORK_WORD_BITS} network-hop-cycles=${NETWORK_HOP_CYCLES} network-contention-model=${NETWORK_CONTENTION_MODEL}"
if [[ -n "${COST_PROFILE}" ]]; then
    mapping_options+=" cost-profile=${COST_PROFILE}"
fi
if [[ "${VERIFY_PLAN}" == true || "${VERIFY_PLAN}" == 1 ]]; then
    mapping_options+=" verify-plan"
fi
if [[ -n "${DIGITAL_SCHEDULING_POLICY}" ]]; then
    mapping_options+=" digital-scheduling-policy=${DIGITAL_SCHEDULING_POLICY}"
    if [[ "${DIGITAL_SCHEDULING_POLICY}" == sliding-window ]]; then
        mapping_options+=" digital-window-size=${DIGITAL_WINDOW_SIZE}"
    fi
elif [[ "${BALANCE_DIGITAL_WORK}" == 1 ]]; then
    mapping_options+=" balance-digital-work"
fi
run_stage "${OUTPUT_DIR}/05-ra-tree.mlir" "${OUTPUT_DIR}/06-mapping-plan.mlir" \
    "sculptor-plan-mapping{${mapping_options}}"
if [[ "${STOP_AFTER_STAGE}" == 06-mapping-plan ]]; then
    echo "stopped after RA-tree mapping plan: ${OUTPUT_DIR}/06-mapping-plan.mlir"
    exit 0
fi
# The current tile outliner consumes the RA tree and logical-tile graph that
# plan-mapping emits.  apply-mapping-plan intentionally consumes those
# attributes, so it is not a deployment stage yet.
placement_options="schedule=${PLACEMENT_SCHEDULE} objective=${PLACEMENT_OBJECTIVE} network-mode=${TEMPORAL_NETWORK_MODE} timing-scope=${TIMING_SCOPE} temporal-candidate-limit=${TEMPORAL_CANDIDATE_LIMIT} mesh-rows=${MESH_ROWS} mesh-cols=${MESH_COLS} arrays-per-core=${ARRAYS_PER_CORE} tile-memory-capacity-bytes=${TILE_MEMORY_CAPACITY_BYTES} random-seed=${RANDOM_SEED} summary-output=${OUTPUT_DIR}/placement-summary.csv"
if [[ "${PLACEMENT_SCHEDULE}" == greedy ]]; then
    placement_options+=" greedy-tile-order=${GREEDY_TILE_ORDER} greedy-priority-mode=${GREEDY_PRIORITY_MODE} greedy-candidate-scope=${GREEDY_CANDIDATE_SCOPE} greedy-lookahead=${GREEDY_LOOKAHEAD}"
fi
if [[ "${VERIFY_PLACEMENT}" == true || "${VERIFY_PLACEMENT}" == 1 ]]; then
    placement_options+=" verify-placement"
fi
run_stage "${OUTPUT_DIR}/06-mapping-plan.mlir" "${OUTPUT_DIR}/08-placed.mlir" \
    "sculptor-place-logical-tiles{${placement_options}}"
if [[ "${STOP_AFTER_STAGE}" == 08-placed ]]; then
    echo "stopped after logical-tile placement: ${OUTPUT_DIR}/08-placed.mlir"
    exit 0
fi
outline_options="sequence-waves-in-flight=${SEQUENCE_WAVES_IN_FLIGHT} specialize-conv-patch-generation=${SPECIALIZE_CONV_PATCH_GENERATION}"
outline_options+=" contract-scalar-execution-regions=${CONTRACT_SCALAR_EXECUTION_REGIONS} retain-proved-local-owners=${RETAIN_PROVED_LOCAL_OWNERS} exact-ram-readiness=${EXACT_RAM_READINESS}"
if [[ "${FUSE_PRODUCER_CONSUMER}" == 1 ]]; then
    outline_options+=" fuse-producer-consumer"
fi
if [[ "${CONSOLIDATE_LAYER_REGIONS}" == 1 ]]; then
    outline_options+=" consolidate-layer-regions"
fi
if [[ "${RETAIN_PROVED_LOCAL_OWNERS}" == 1 ]]; then
    outline_options+=" retain-proved-local-owners"
fi
if [[ -n "${RETAINED_OWNER_BOUNDARY_IDS}" ]]; then
    outline_options+=" retain-proved-local-owner-boundary-ids=${RETAINED_OWNER_BOUNDARY_IDS}"
fi
if [[ "${EXACT_RAM_READINESS}" == 1 ]]; then
    outline_options+=" exact-ram-readiness"
fi
if [[ -n "${PRE_SPLIT_CORE_DIR}" ]]; then
    run_outline_split_stage "${OUTPUT_DIR}/08-placed.mlir"
    echo "created RA-tree tile deployment: ${PRE_SPLIT_CORE_DIR}"
else
    run_stage "${OUTPUT_DIR}/08-placed.mlir" \
        "${OUTPUT_DIR}/09-tile-deployment.mlir" "sculptor-outline-tile-routines{${outline_options}}"
    echo "created RA-tree tile deployment: ${OUTPUT_DIR}/09-tile-deployment.mlir"
fi
