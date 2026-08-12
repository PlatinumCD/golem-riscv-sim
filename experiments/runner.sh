#!/usr/bin/env bash
set -euo pipefail

# Shared repository and experiment configuration

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../build-scripts/common.sh
source "${SCRIPT_DIR}/../build-scripts/common.sh"
# shellcheck source=params.sh
source "${SCRIPT_DIR}/params.sh"

# Command-line options

usage() {
    cat >&2 <<EOF
usage: $0 [--run] [--trace] [--resume-build] [--remove-intermediate-mlir]
  --run                       Run the SST simulation after the build.
  --trace                     Record detailed trace files during the simulation.
  --resume-build              Use the existing deployment and core objects.
  --remove-intermediate-mlir  Remove all generated MLIR files after success.
EOF
}

RUN_EXPERIMENT=false
TRACE_EXPERIMENT=false
RESUME_BUILD=false
REMOVE_INTERMEDIATE_MLIR=false
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --run)
            RUN_EXPERIMENT=true
            shift
            ;;
        --trace)
            TRACE_EXPERIMENT=true
            shift
            ;;
        --resume-build)
            RESUME_BUILD=true
            shift
            ;;
        --remove-intermediate-mlir)
            REMOVE_INTERMEDIATE_MLIR=true
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
readonly RUN_EXPERIMENT
readonly TRACE_EXPERIMENT
readonly RESUME_BUILD
readonly REMOVE_INTERMEDIATE_MLIR

if [[ "${TRACE_EXPERIMENT}" == true ]]; then
    if [[ "${RUN_EXPERIMENT}" != true ]]; then
        echo "--trace requires --run" >&2
        usage
        exit 2
    fi
    GOLEM_MODEL_PROFILE_MODE=trace
fi

# GPT-2 generator and output paths

readonly GENERATOR="${SCRIPT_DIR}/models/gpt2_generator.py"
readonly OUTPUT_DIR="${GOLEM_EXPERIMENT_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2}"
readonly MODEL_MLIR="${OUTPUT_DIR}/model.mlir"
readonly COMPILER_DIR="${OUTPUT_DIR}/compiler"
readonly FINAL_MLIR="${COMPILER_DIR}/08-placed.mlir"
readonly DEPLOYMENT_MLIR="${COMPILER_DIR}/09-tile-deployment.mlir"
readonly CORE_OBJECT_DIR="${COMPILER_DIR}/cores"
readonly ACTIVE_CORE_MANIFEST="${OUTPUT_DIR}/active-cores.txt"
readonly ELF_DIR="${OUTPUT_DIR}/elf"
readonly EXPERIMENT_SUPPORT_DIR="${PROJECT_ROOT}/tests/models/sculptor-ra-tree"
readonly TORCH_MLIR_PACKAGE="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly TORCH_MLIR_OPT_BIN="${INSTALL_ROOT}/torch-mlir/bin/torch-mlir-opt"
readonly SCULPTOR_OPT_BIN="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly LLVM_ROOT="${INSTALL_ROOT}/llvm"
readonly CLANG="${LLVM_ROOT}/bin/clang"
readonly CLANGXX="${LLVM_ROOT}/bin/clang++"
readonly LLVM_READELF="${LLVM_ROOT}/bin/llvm-readelf"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly SIMULATION_CONFIG="${PROJECT_ROOT}/tests/models/sculptor-ra-tree/simulation.py"
readonly SIMULATION_LOG="${OUTPUT_DIR}/simulation.log"
readonly STATS="${OUTPUT_DIR}/router-statistics.csv"
readonly RESULT="${OUTPUT_DIR}/result.csv"
readonly TRACE_DIR="${OUTPUT_DIR}/trace"
readonly TILE_MAIN_SOURCE="${PROJECT_ROOT}/platform/sculptor-tile-main.cpp"

# Required tools and inputs

require_executable "${COMPILER_PYTHON}"
require_executable "${TORCH_MLIR_OPT_BIN}"
require_executable "${SCULPTOR_OPT_BIN}"
require_executable "${CLANG}"
require_executable "${CLANGXX}"
require_executable "${LLVM_READELF}"
require_file "${GENERATOR}"
require_file "${TILE_MAIN_SOURCE}"
require_file "${EXPERIMENT_SUPPORT_DIR}/idle-main.cpp"
require_file "${EXPERIMENT_SUPPORT_DIR}/tile-model-suite.ld"
if [[ "${RUN_EXPERIMENT}" == true ]]; then
    require_executable "${QEMU}"
    require_executable "${SST}"
    require_file "${SIMULATION_CONFIG}"
fi

# Parameter validation

require_boolean() {
    local name="$1"
    local value="$2"

    case "${value}" in
        true|false) ;;
        *)
            echo "${name} must be true or false" >&2
            exit 2
            ;;
    esac
}

require_positive_integer() {
    local name="$1"
    local value="$2"

    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "${name} must be a positive integer" >&2
        exit 2
    fi
}

require_boolean SCULPTOR_DUPLICATE_MATRICES "${SCULPTOR_DUPLICATE_MATRICES}"
require_boolean SCULPTOR_REQUIRE_CHANGE "${SCULPTOR_REQUIRE_CHANGE}"
require_boolean SCULPTOR_REQUIRE_COMPLETE_SHARD_CHAIN "${SCULPTOR_REQUIRE_COMPLETE_SHARD_CHAIN}"
require_boolean SCULPTOR_BALANCE_DIGITAL_WORK "${SCULPTOR_BALANCE_DIGITAL_WORK}"
require_boolean SCULPTOR_VERIFY_PLAN "${SCULPTOR_VERIFY_PLAN}"
require_boolean SCULPTOR_VERIFY_PLACEMENT "${SCULPTOR_VERIFY_PLACEMENT}"
case "${SCULPTOR_DATAFLOW}" in
    bulk|sharded) ;;
    *)
        echo "SCULPTOR_DATAFLOW must be bulk or sharded" >&2
        exit 2
        ;;
esac
case "${SCULPTOR_REDUCTION_TREE}" in
    none|balanced) ;;
    *)
        echo "SCULPTOR_REDUCTION_TREE must be none or balanced" >&2
        exit 2
        ;;
esac
case "${SCULPTOR_PLACEMENT_OBJECTIVE}" in
    transfer-cost|makespan) ;;
    *)
        echo "SCULPTOR_PLACEMENT_OBJECTIVE must be transfer-cost or makespan" >&2
        exit 2
        ;;
esac
case "${SCULPTOR_TEMPORAL_NETWORK_MODE}" in
    ideal|finite|full) ;;
    *)
        echo "SCULPTOR_TEMPORAL_NETWORK_MODE must be ideal, finite, or full" >&2
        exit 2
        ;;
esac
case "${SCULPTOR_TIMING_SCOPE}" in
    warm|cold) ;;
    *)
        echo "SCULPTOR_TIMING_SCOPE must be warm or cold" >&2
        exit 2
        ;;
esac
if [[ -n "${SCULPTOR_COST_PROFILE}" ]]; then
    require_file "${SCULPTOR_COST_PROFILE}"
fi
case "${GOLEM_MODEL_MESH_ROUTER_BACKEND}" in
    merlin|mittens) ;;
    *)
        echo "GOLEM_MODEL_MESH_ROUTER_BACKEND must be merlin or mittens" >&2
        exit 2
        ;;
esac
for parameter_name in \
    GOLEM_MODEL_WORMHOLE_INPUT_BUFFER_FLITS \
    GOLEM_MODEL_WORMHOLE_INJECTION_BUFFER_FLITS \
    GOLEM_MODEL_WORMHOLE_PIPELINE_CYCLES; do
    require_positive_integer "${parameter_name}" "${!parameter_name}"
done
for parameter_name in \
    GPT2_NUM_DECODERS \
    GPT2_SEQUENCE_LENGTH \
    GPT2_HIDDEN_SIZE \
    GPT2_ATTENTION_HEADS \
    GPT2_INTERMEDIATE_SIZE \
    SCULPTOR_MESH_ROWS \
    SCULPTOR_MESH_COLS \
    SCULPTOR_PARALLEL_WORKERS \
    SCULPTOR_DIGITAL_ISSUE_WIDTH \
    SCULPTOR_NETWORK_WORD_BITS \
    SCULPTOR_REDUCTION_FAN_IN \
    SCULPTOR_REDUCTION_MIN_WIDTH \
    SCULPTOR_TEMPORAL_CANDIDATE_LIMIT; do
    require_positive_integer "${parameter_name}" "${!parameter_name}"
done
if [[ ! "${SCULPTOR_SHARD_PROPAGATION_DEPTH}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_SHARD_PROPAGATION_DEPTH must be a nonnegative integer" >&2
    exit 2
fi
if ((SCULPTOR_NETWORK_WORD_BITS % 32 != 0)); then
    echo "SCULPTOR_NETWORK_WORD_BITS must be a multiple of 32" >&2
    exit 2
fi
if [[ "${RUN_EXPERIMENT}" == true ]]; then
    case "${GOLEM_MODEL_PROFILE_MODE}" in
        off|summary|trace) ;;
        *)
            echo "GOLEM_MODEL_PROFILE_MODE must be off, summary, or trace" >&2
            exit 2
            ;;
    esac
    for value in "${GOLEM_MODEL_SST_THREADS}" \
        "${GOLEM_MODEL_SYNC_INSTRUCTION_QUANTUM}"; do
        if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
            echo "SST threads and the synchronization quantum must be positive integers" >&2
            exit 2
        fi
    done
    case "${GOLEM_MODEL_SST_PARTITIONER}" in
        sst.simple|sst.linear|sst.roundrobin) ;;
        *)
            echo "GOLEM_MODEL_SST_PARTITIONER must be sst.simple, sst.linear, or sst.roundrobin" >&2
            exit 2
            ;;
    esac
    case "${GOLEM_MODEL_MEMORY_BACKEND}" in
        native|memhierarchy) ;;
        *)
            echo "GOLEM_MODEL_MEMORY_BACKEND must be native or memhierarchy" >&2
            exit 2
            ;;
    esac
fi

# Run manifest

mkdir -p -- "${OUTPUT_DIR}" "${COMPILER_DIR}" "${CORE_OBJECT_DIR}" "${ELF_DIR}"
rm -f -- "${OUTPUT_DIR}/.complete"

{
    declare -p RUN_EXPERIMENT
    declare -p TRACE_EXPERIMENT
    declare -p RESUME_BUILD
    declare -p REMOVE_INTERMEDIATE_MLIR
    while IFS= read -r parameter; do
        declare -p "${parameter}"
    done < <(
        compgen -A variable |
            grep -E '^(GPT2_|SCULPTOR_|GOLEM_MODEL_)' |
            sort
    )
} >"${OUTPUT_DIR}/parameters.env"

# Model generation

if [[ "${RESUME_BUILD}" == false ]]; then
TORCH_MLIR_PYTHON_PACKAGE="${TORCH_MLIR_PACKAGE}" \
TORCH_MLIR_OPT="${TORCH_MLIR_OPT_BIN}" \
    "${COMPILER_PYTHON}" "${GENERATOR}" \
        --mode mlir \
        --layers "${GPT2_NUM_DECODERS}" \
        --sequence-length "${GPT2_SEQUENCE_LENGTH}" \
        --hidden-size "${GPT2_HIDDEN_SIZE}" \
        --attention-heads "${GPT2_ATTENTION_HEADS}" \
        --intermediate-size "${GPT2_INTERMEDIATE_SIZE}" \
        --output "${MODEL_MLIR}"

echo "generated GPT-2 MLIR: ${MODEL_MLIR}"

# Compiler pipeline

run_stage() {
    local label="$1"
    local input="$2"
    local output="$3"
    shift 3

    echo "[compiler] ${label}"
    "${SCULPTOR_OPT_BIN}" "${input}" --verify-each "$@" -o "${output}"
}

run_stage "canonicalize and extract layers" \
    "${MODEL_MLIR}" "${COMPILER_DIR}/01-canonical.mlir" \
    --sculptor-canonicalize-layers \
    --sculptor-extract-layers

run_stage "convert layers" \
    "${COMPILER_DIR}/01-canonical.mlir" \
    "${COMPILER_DIR}/02-converted.mlir" \
    --sculptor-convert-layers

run_stage "expand MVM operations" \
    "${COMPILER_DIR}/02-converted.mlir" \
    "${COMPILER_DIR}/03-golem.mlir" \
    "--sculptor-expand-mvm-to-golem=array-rows=${SCULPTOR_ARRAY_ROWS} array-cols=${SCULPTOR_ARRAY_COLS}"

if [[ "${SCULPTOR_DUPLICATE_MATRICES}" == true ]]; then
    run_stage "duplicate matrices" \
        "${COMPILER_DIR}/03-golem.mlir" \
        "${COMPILER_DIR}/04-matrices.mlir" \
        --sculptor-duplicate-matrices
else
    cp -- "${COMPILER_DIR}/03-golem.mlir" \
        "${COMPILER_DIR}/04-matrices.mlir"
fi

digital_work_options="parallel-workers=${SCULPTOR_PARALLEL_WORKERS}"
digital_work_options+=" dataflow=${SCULPTOR_DATAFLOW}"
digital_work_options+=" shard-propagation-depth=${SCULPTOR_SHARD_PROPAGATION_DEPTH}"
digital_work_options+=" reduction-tree=${SCULPTOR_REDUCTION_TREE}"
digital_work_options+=" reduction-fan-in=${SCULPTOR_REDUCTION_FAN_IN}"
digital_work_options+=" reduction-min-width=${SCULPTOR_REDUCTION_MIN_WIDTH}"
if [[ "${SCULPTOR_REQUIRE_CHANGE}" == true ]]; then
    digital_work_options+=" require-change"
fi
if [[ "${SCULPTOR_REQUIRE_COMPLETE_SHARD_CHAIN}" == true ]]; then
    digital_work_options+=" require-complete-shard-chain"
fi
run_stage "expand digital work" \
    "${COMPILER_DIR}/04-matrices.mlir" \
    "${COMPILER_DIR}/05-digital-work.mlir" \
    "--sculptor-expand-digital-work=${digital_work_options}"

run_stage "build RA tree" \
    "${COMPILER_DIR}/05-digital-work.mlir" \
    "${COMPILER_DIR}/06-ra-tree.mlir" \
    --sculptor-build-ra-tree

mapping_options=(
    "strategies=${SCULPTOR_PLANNER_STRATEGIES}"
    "mvm-body-policy=${SCULPTOR_MVM_BODY_POLICY}"
    "setup-binding-policy=${SCULPTOR_SETUP_BINDING_POLICY}"
    "objective=${SCULPTOR_OBJECTIVE}"
    "mesh-rows=${SCULPTOR_MESH_ROWS}"
    "mesh-cols=${SCULPTOR_MESH_COLS}"
    "arrays-per-core=${SCULPTOR_ARRAYS_PER_CORE}"
    "array-rows=${SCULPTOR_ARRAY_ROWS}"
    "array-cols=${SCULPTOR_ARRAY_COLS}"
    "local-memory-bytes-per-core=${SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE}"
    "clock-frequency-hz=${SCULPTOR_CLOCK_FREQUENCY_HZ}"
    "analog-mvm-latency-ns=${SCULPTOR_ANALOG_MVM_LATENCY_NS}"
    "analog-io-bits-per-cycle=${SCULPTOR_ANALOG_IO_BITS_PER_CYCLE}"
    "analog-io-policy=${SCULPTOR_ANALOG_IO_POLICY}"
    "analog-array-execution=${SCULPTOR_ANALOG_ARRAY_EXECUTION}"
    "digital-issue-width=${SCULPTOR_DIGITAL_ISSUE_WIDTH}"
    "digital-vector-bits-per-cycle=${SCULPTOR_DIGITAL_VECTOR_BITS_PER_CYCLE}"
    "network-word-bits=${SCULPTOR_NETWORK_WORD_BITS}"
    "network-hop-cycles=${SCULPTOR_NETWORK_HOP_CYCLES}"
    "network-contention-model=${SCULPTOR_NETWORK_CONTENTION_MODEL}"
)
if [[ "${SCULPTOR_BALANCE_DIGITAL_WORK}" == true ]]; then
    mapping_options+=("balance-digital-work")
fi
if [[ -n "${SCULPTOR_COST_PROFILE}" ]]; then
    mapping_options+=("cost-profile=${SCULPTOR_COST_PROFILE}")
fi
if [[ "${SCULPTOR_VERIFY_PLAN}" == true ]]; then
    mapping_options+=("verify-plan")
fi
run_stage "plan logical mapping" \
    "${COMPILER_DIR}/06-ra-tree.mlir" \
    "${COMPILER_DIR}/07-mapping-plan.mlir" \
    "--sculptor-plan-mapping=${mapping_options[*]}"

placement_options=(
    "schedule=${SCULPTOR_PLACEMENT_SCHEDULE}"
    "objective=${SCULPTOR_PLACEMENT_OBJECTIVE}"
    "network-mode=${SCULPTOR_TEMPORAL_NETWORK_MODE}"
    "timing-scope=${SCULPTOR_TIMING_SCOPE}"
    "temporal-candidate-limit=${SCULPTOR_TEMPORAL_CANDIDATE_LIMIT}"
    "mesh-rows=${SCULPTOR_MESH_ROWS}"
    "mesh-cols=${SCULPTOR_MESH_COLS}"
    "arrays-per-core=${SCULPTOR_ARRAYS_PER_CORE}"
    "random-seed=${SCULPTOR_RANDOM_SEED}"
    "summary-output=${COMPILER_DIR}/placement-summary.csv"
)
if [[ "${SCULPTOR_PLACEMENT_SCHEDULE}" == greedy ]]; then
    placement_options+=(
        "greedy-tile-order=${SCULPTOR_GREEDY_TILE_ORDER}"
        "greedy-priority-mode=${SCULPTOR_GREEDY_PRIORITY_MODE}"
        "greedy-candidate-scope=${SCULPTOR_GREEDY_CANDIDATE_SCOPE}"
        "greedy-lookahead=${SCULPTOR_GREEDY_LOOKAHEAD}"
    )
fi
if [[ "${SCULPTOR_VERIFY_PLACEMENT}" == true ]]; then
    placement_options+=("verify-placement")
fi
run_stage "place logical tiles" \
    "${COMPILER_DIR}/07-mapping-plan.mlir" "${FINAL_MLIR}" \
    "--sculptor-place-logical-tiles=${placement_options[*]}"

echo "compiled GPT-2 placement: ${FINAL_MLIR}"

# Tile deployment and core objects

run_stage "outline tile routines" \
    "${FINAL_MLIR}" "${DEPLOYMENT_MLIR}" \
    --sculptor-outline-tile-routines

"${PROJECT_ROOT}/build-scripts/build-runtime.sh"

SCULPTOR_DEPLOYMENT_MLIR="${DEPLOYMENT_MLIR}" \
SCULPTOR_CORE_OBJECT_DIR="${CORE_OBJECT_DIR}" \
SCULPTOR_ACTIVE_CORE_MANIFEST="${ACTIVE_CORE_MANIFEST}" \
SCULPTOR_CORE_BUILD_JOBS="${BUILD_JOBS}" \
SCULPTOR_CORE_LTO=none \
SCULPTOR_CORE_REGALLOC_FALLBACK="${SCULPTOR_CORE_REGALLOC_FALLBACK}" \
    "${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"
else
    require_file "${ACTIVE_CORE_MANIFEST}"
    if [[ ! -d "${CORE_OBJECT_DIR}" ]]; then
        echo "missing required directory: ${CORE_OBJECT_DIR}" >&2
        exit 1
    fi
    echo "reusing compiled core objects: ${CORE_OBJECT_DIR}"
fi

# Bare-metal ELF generation

common_flags=(
    "--target=${GOLEM_TARGET}"
    "-mcpu=${GOLEM_CPU}"
    "-mabi=${GOLEM_ABI}"
    -mcmodel=medany
    -ffreestanding
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    -O2
)
cxx_flags=(
    "${common_flags[@]}"
    -std=c++20
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    "-I${RUNTIME_INCLUDE}"
    "-I${PROJECT_ROOT}/platform"
)

"${CLANG}" "${common_flags[@]}" -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${ELF_DIR}/crt0.o"
for source in uart platform-exit freestanding-memory freestanding-math; do
"${CLANGXX}" "${cxx_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/${source}.cpp" \
    -o "${ELF_DIR}/${source}.o"
done
tile_main_flags=("${cxx_flags[@]}")
if [[ "${TRACE_EXPERIMENT}" == true ]]; then
    tile_main_flags+=(-DGOLEM_ENABLE_TASK_TRACE=1)
fi
"${CLANGXX}" "${tile_main_flags[@]}" \
    -c "${TILE_MAIN_SOURCE}" \
    -o "${ELF_DIR}/tile-main.o"
"${CLANGXX}" "${cxx_flags[@]}" \
    -c "${EXPERIMENT_SUPPORT_DIR}/idle-main.cpp" \
    -o "${ELF_DIR}/idle-main.o"

sed "s/__GOLEM_TILE_MEMORY__/${SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE}/" \
    "${EXPERIMENT_SUPPORT_DIR}/tile-model-suite.ld" \
    >"${ELF_DIR}/tile.ld"

link_tile() {
    local output="$1"
    local main_object="$2"
    local generated_object="${3:-}"
    local -a objects=(
        "${ELF_DIR}/crt0.o"
        "${ELF_DIR}/uart.o"
        "${ELF_DIR}/platform-exit.o"
        "${ELF_DIR}/freestanding-memory.o"
        "${ELF_DIR}/freestanding-math.o"
        "${main_object}"
    )

    if [[ -n "${generated_object}" ]]; then
        objects+=("${generated_object}" "${RUNTIME_LIBRARY}")
    fi
    rm -f -- "${output}"
    "${CLANGXX}" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs -fuse-ld=lld \
        -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${ELF_DIR}/tile.ld" \
        "${objects[@]}" -o "${output}"
    local elf_header
    elf_header="$("${LLVM_READELF}" -h "${output}")"
    grep -q 'Machine:.*RISC-V' <<<"${elf_header}"
}

link_tile "${ELF_DIR}/idle.elf" "${ELF_DIR}/idle-main.o"

readonly PHYSICAL_TILE_COUNT=$((SCULPTOR_MESH_ROWS * SCULPTOR_MESH_COLS))
mapfile -t ACTIVE_CORES <"${ACTIVE_CORE_MANIFEST}"
if [[ "${#ACTIVE_CORES[@]}" -eq 0 ]]; then
    echo "deployment did not generate active tiles" >&2
    exit 1
fi
declare -A ACTIVE_CORE_SET=()
for core_id in "${ACTIVE_CORES[@]}"; do
    if [[ ! "${core_id}" =~ ^(0|[1-9][0-9]*)$ ]] ||
       ((core_id >= PHYSICAL_TILE_COUNT)); then
        echo "active core ID is outside the physical mesh: ${core_id}" >&2
        exit 1
    fi
    if [[ -n "${ACTIVE_CORE_SET[${core_id}]:-}" ]]; then
        echo "active core manifest contains duplicate ID: ${core_id}" >&2
        exit 1
    fi
    require_file "${CORE_OBJECT_DIR}/core-${core_id}.o"
    ACTIVE_CORE_SET["${core_id}"]=1
done

for ((tile_id = 0; tile_id < PHYSICAL_TILE_COUNT; ++tile_id)); do
    core_object="${CORE_OBJECT_DIR}/core-${tile_id}.o"
    tile_elf="${ELF_DIR}/tile-${tile_id}.elf"
    if [[ -n "${ACTIVE_CORE_SET[${tile_id}]:-}" ]]; then
        link_tile "${tile_elf}" "${ELF_DIR}/tile-main.o" "${core_object}"
    else
        rm -f -- "${tile_elf}"
        ln -s -- idle.elf "${tile_elf}"
    fi
done

for core_id in "${ACTIVE_CORES[@]}"; do
    require_file "${ELF_DIR}/tile-${core_id}.elf"
done

echo "linked ${#ACTIVE_CORES[@]} runnable and ${PHYSICAL_TILE_COUNT} total tile ELFs: ${ELF_DIR}"

# Optional SST and QEMU execution

if [[ "${RUN_EXPERIMENT}" == true ]]; then
    readonly CPU_CLOCK="${SCULPTOR_CLOCK_FREQUENCY_HZ}Hz"
    readonly TILE_MEMORY="${SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE}B"
    readonly ANALOG_COMPUTE_LATENCY_CYCLES=$((
        (SCULPTOR_ANALOG_MVM_LATENCY_NS * SCULPTOR_CLOCK_FREQUENCY_HZ +
         999999999) / 1000000000
    ))

    # QEMU allocates the configured guest RAM for every runnable tile.  Refuse
    # a launch that can exhaust the host, and put SST and all QEMU children in
    # an independent systemd scope so an OOM cannot kill the caller's tmux
    # session.
    readonly HOST_AVAILABLE_BYTES=$(awk '/^MemAvailable:/ { print $2 * 1024 }' /proc/meminfo)
    readonly ESTIMATED_RUN_BYTES=$((
        ${#ACTIVE_CORES[@]} *
        (SCULPTOR_LOCAL_MEMORY_BYTES_PER_CORE + GOLEM_MODEL_QEMU_HOST_OVERHEAD_BYTES)
    ))
    readonly HOST_RUN_BUDGET_BYTES=$((
        HOST_AVAILABLE_BYTES - GOLEM_MODEL_HOST_MEMORY_RESERVE_BYTES
    ))
    if ((HOST_RUN_BUDGET_BYTES <= 0)); then
        echo "insufficient available host memory after the configured reserve" >&2
        exit 1
    fi
    if ((ESTIMATED_RUN_BYTES > HOST_RUN_BUDGET_BYTES)); then
        echo "unsafe simulation memory estimate: ${ESTIMATED_RUN_BYTES} bytes required, ${HOST_RUN_BUDGET_BYTES} bytes available after reserve" >&2
        exit 1
    fi
    if [[ "${GOLEM_MODEL_ISOLATE_SIMULATION}" == true ]] &&
       ((ESTIMATED_RUN_BYTES > GOLEM_MODEL_RUN_MEMORY_MAX_BYTES)); then
        echo "simulation estimate exceeds isolated scope limit: ${ESTIMATED_RUN_BYTES} > ${GOLEM_MODEL_RUN_MEMORY_MAX_BYTES} bytes" >&2
        exit 1
    fi
    echo "simulation memory preflight: ${ESTIMATED_RUN_BYTES} estimated bytes, ${HOST_RUN_BUDGET_BYTES} host-budget bytes"

    if [[ "${GOLEM_MODEL_PROFILE_MODE}" != off ]]; then
        mkdir -p -- "${TRACE_DIR}/performance"
    fi
    if [[ "${TRACE_EXPERIMENT}" == true ]]; then
        mkdir -p -- "${TRACE_DIR}/tasks"
    fi

    export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
    export MITTENS_TEST_QEMU="${QEMU}"
    export MITTENS_SCULPTOR_ELF_DIRECTORY="${ELF_DIR}"
    export MITTENS_SCULPTOR_ACTIVE_CORES="$(IFS=,; echo "${ACTIVE_CORES[*]}")"
    export MITTENS_SCULPTOR_STATS="${STATS}"
    export MITTENS_SCULPTOR_MESH_ROWS="${SCULPTOR_MESH_ROWS}"
    export MITTENS_SCULPTOR_MESH_COLS="${SCULPTOR_MESH_COLS}"
    export MITTENS_ANALOG_ARRAY_COUNT="${SCULPTOR_ARRAYS_PER_CORE}"
    export MITTENS_ANALOG_ARRAY_ROWS="${SCULPTOR_ARRAY_ROWS}"
    export MITTENS_ANALOG_ARRAY_COLUMNS="${SCULPTOR_ARRAY_COLS}"
    export MITTENS_SCULPTOR_MEMORY_BACKEND="${GOLEM_MODEL_MEMORY_BACKEND}"
    export MITTENS_SCULPTOR_TILE_MEMORY="${TILE_MEMORY}"
    export MITTENS_SCULPTOR_CPU_CLOCK="${CPU_CLOCK}"
    export MITTENS_SCULPTOR_CPU_ISSUE_WIDTH="${SCULPTOR_DIGITAL_ISSUE_WIDTH}"
    export MITTENS_SCULPTOR_SYNC_INSTRUCTION_QUANTUM="${GOLEM_MODEL_SYNC_INSTRUCTION_QUANTUM}"
    export MITTENS_SCULPTOR_ANALOG_BACKEND="${GOLEM_MODEL_ANALOG_BACKEND}"
    export MITTENS_SCULPTOR_ANALOG_LINK_CLOCK="${CPU_CLOCK}"
    export MITTENS_SCULPTOR_ANALOG_COMPUTE_LATENCY_CYCLES="${ANALOG_COMPUTE_LATENCY_CYCLES}"
    export MITTENS_SCULPTOR_MESH_LINK_WIDTH_BITS="${SCULPTOR_NETWORK_WORD_BITS}"
    export MITTENS_SCULPTOR_MESH_LINK_CLOCK="${CPU_CLOCK}"
    export MITTENS_SCULPTOR_MESH_LINK_LATENCY="${GOLEM_MODEL_MESH_LINK_LATENCY}"
    export MITTENS_SCULPTOR_MESH_ROUTER_BACKEND="${GOLEM_MODEL_MESH_ROUTER_BACKEND}"
    export MITTENS_SCULPTOR_WORMHOLE_INPUT_BUFFER_FLITS="${GOLEM_MODEL_WORMHOLE_INPUT_BUFFER_FLITS}"
    export MITTENS_SCULPTOR_WORMHOLE_INJECTION_BUFFER_FLITS="${GOLEM_MODEL_WORMHOLE_INJECTION_BUFFER_FLITS}"
    export MITTENS_SCULPTOR_WORMHOLE_PIPELINE_CYCLES="${GOLEM_MODEL_WORMHOLE_PIPELINE_CYCLES}"
    export MITTENS_SCULPTOR_NETWORK_CELL_WORDS="${GOLEM_MODEL_NETWORK_CELL_WORDS}"
    export MITTENS_SCULPTOR_NETWORK_BUFFER_CELLS="${GOLEM_MODEL_NETWORK_BUFFER_CELLS}"
    export MITTENS_SCULPTOR_NETWORK_PACKET_WORDS="${GOLEM_MODEL_NETWORK_PACKET_WORDS}"
    export MITTENS_SCULPTOR_RX_DMA_WIDTH_BITS="${GOLEM_MODEL_RX_DMA_WIDTH_BITS}"
    export MITTENS_SCULPTOR_RX_DMA_SETUP_CYCLES="${GOLEM_MODEL_RX_DMA_SETUP_CYCLES}"
    export MITTENS_SCULPTOR_RX_DMA_QUEUE_DEPTH="${GOLEM_MODEL_RX_DMA_QUEUE_DEPTH}"
    export MITTENS_SCULPTOR_RVV_ENABLED="${GOLEM_MODEL_RVV_ENABLED}"
    export MITTENS_SCULPTOR_RVV_LENGTH_BITS="${GOLEM_MODEL_RVV_LENGTH_BITS}"
    export MITTENS_SCULPTOR_RVV_ELEMENT_BITS="${GOLEM_MODEL_RVV_ELEMENT_BITS}"
    export MITTENS_SCULPTOR_SCRATCHPAD_ENABLED="${GOLEM_MODEL_SCRATCHPAD_ENABLED}"
    export MITTENS_SCULPTOR_PROFILE_MODE="${GOLEM_MODEL_PROFILE_MODE}"
    export MITTENS_SCULPTOR_PROFILE_OUTPUT_DIRECTORY="${TRACE_DIR}/performance"
    if [[ "${TRACE_EXPERIMENT}" == true ]]; then
        export MITTENS_SCULPTOR_TASK_TRACE_DIRECTORY="${TRACE_DIR}/tasks"
    else
        export MITTENS_SCULPTOR_TASK_TRACE_DIRECTORY=""
    fi
    export MITTENS_SCULPTOR_MEMORY_INIT_BATCHING="${GOLEM_MODEL_MEMORY_INIT_BATCHING}"
    export MITTENS_SCULPTOR_MEMORY_ACCESS_BATCHING="${GOLEM_MODEL_MEMORY_ACCESS_BATCHING}"
    export MITTENS_SCULPTOR_MEMORY_ACCESS_BATCH_RECORDS="${GOLEM_MODEL_MEMORY_ACCESS_BATCH_RECORDS}"
    export MITTENS_SCULPTOR_MEMORY_LOAD_QUEUE_ENTRIES="${GOLEM_MODEL_MEMORY_LOAD_QUEUE_ENTRIES}"
    export MITTENS_SCULPTOR_MEMORY_STORE_BUFFER_ENTRIES="${GOLEM_MODEL_MEMORY_STORE_BUFFER_ENTRIES}"
    export MITTENS_SCULPTOR_MEMORY_INIT_BYTES_PER_CYCLE="${GOLEM_MODEL_MEMORY_INIT_BYTES_PER_CYCLE}"
    export MITTENS_SCULPTOR_MEMORY_INIT_LATENCY_CYCLES="${GOLEM_MODEL_MEMORY_INIT_LATENCY_CYCLES}"
    export MITTENS_SCULPTOR_L1_SIZE="${GOLEM_MODEL_L1_SIZE}"
    export MITTENS_SCULPTOR_L1_ASSOCIATIVITY="${GOLEM_MODEL_L1_ASSOCIATIVITY}"
    export MITTENS_SCULPTOR_CACHE_LINE_SIZE="${GOLEM_MODEL_CACHE_LINE_SIZE}"
    export MITTENS_SCULPTOR_L1_ACCESS_LATENCY_CYCLES="${GOLEM_MODEL_L1_ACCESS_LATENCY_CYCLES}"
    export MITTENS_SCULPTOR_L1_MAX_REQUESTS_PER_CYCLE="${GOLEM_MODEL_L1_MAX_REQUESTS_PER_CYCLE}"
    export MITTENS_SCULPTOR_L1_BANKS="${GOLEM_MODEL_L1_BANKS}"
    export MITTENS_SCULPTOR_L1_CLOCK="${GOLEM_MODEL_L1_CLOCK}"
    export MITTENS_SCULPTOR_LOWER_MEMORY_CLOCK="${GOLEM_MODEL_LOWER_MEMORY_CLOCK}"
    export MITTENS_SCULPTOR_LOWER_MEMORY_ACCESS_TIME="${GOLEM_MODEL_LOWER_MEMORY_ACCESS_TIME}"

    rm -f -- "${SIMULATION_LOG}" "${STATS}" "${RESULT}"
    simulation_start_ns="$(date +%s%N)"
    sst_command=(
        "${SST}" -n "${GOLEM_MODEL_SST_THREADS}"
        --partitioner="${GOLEM_MODEL_SST_PARTITIONER}"
        "${SIMULATION_CONFIG}"
    )
    if [[ "${GOLEM_MODEL_ISOLATE_SIMULATION}" == true ]]; then
        if ! command -v systemd-run >/dev/null 2>&1; then
            echo "systemd-run is required when GOLEM_MODEL_ISOLATE_SIMULATION=true" >&2
            exit 1
        fi
        scope_name="golem-sim-$BASHPID-$(date +%s)"
        systemd-run --user --scope --quiet \
            --unit="${scope_name}" \
            -p "MemoryMax=${GOLEM_MODEL_RUN_MEMORY_MAX_BYTES}" \
            -p MemorySwapMax=0 \
            -p OOMPolicy=stop \
            -- "${sst_command[@]}" 2>&1 | tee "${SIMULATION_LOG}"
    else
        "${sst_command[@]}" 2>&1 | tee "${SIMULATION_LOG}"
    fi
    simulation_end_ns="$(date +%s%N)"
    simulation_wall_seconds="$(awk -v start="${simulation_start_ns}" \
        -v end="${simulation_end_ns}" \
        'BEGIN { printf "%.6f", (end - start) / 1000000000 }')"

    # Each managed tile is an SST primary component. SST cannot finish until
    # every QEMU process exits and its outgoing traffic drains. The tile
    # component also makes a nonzero QEMU exit fatal. Do not count UART pass
    # strings here because output from parallel SST ranks can interleave at
    # individual characters.
    simulated_time="$(awk '/Simulation is complete/ { print $(NF - 1) " " $NF }' \
        "${SIMULATION_LOG}" | tail -n1)"
    if [[ -z "${simulated_time}" ]]; then
        echo "SST did not report completion time" >&2
        exit 1
    fi

    printf 'mesh,parallel_workers,gpt2_decoders,gpt2_tokens,digital_issue_width,mesh_link_width_bits,router_backend,active_tiles,sst_threads,sst_partitioner,memory_backend,simulated_time,wall_seconds\n%sx%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${SCULPTOR_MESH_ROWS}" "${SCULPTOR_MESH_COLS}" \
        "${SCULPTOR_PARALLEL_WORKERS}" "${GPT2_NUM_DECODERS}" \
        "${GPT2_SEQUENCE_LENGTH}" "${SCULPTOR_DIGITAL_ISSUE_WIDTH}" \
        "${SCULPTOR_NETWORK_WORD_BITS}" "${GOLEM_MODEL_MESH_ROUTER_BACKEND}" \
        "${#ACTIVE_CORES[@]}" \
        "${GOLEM_MODEL_SST_THREADS}" "${GOLEM_MODEL_SST_PARTITIONER}" \
        "${GOLEM_MODEL_MEMORY_BACKEND}" "${simulated_time}" \
        "${simulation_wall_seconds}" >"${RESULT}"
    echo "simulation PASS: ${simulated_time} simulated, ${simulation_wall_seconds}s wall"
fi

if [[ "${REMOVE_INTERMEDIATE_MLIR}" == true ]]; then
    "${SCRIPT_DIR}/remove-intermediate-mlir.sh" "${OUTPUT_DIR}"
fi

date -u +'%Y-%m-%dT%H:%M:%SZ' >"${OUTPUT_DIR}/.complete"
