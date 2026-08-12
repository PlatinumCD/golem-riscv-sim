#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
# shellcheck source=parameters.sh
source "${TEST_DIR}/parameters.sh"

MEMORY_BACKEND="${GOLEM_MODEL_MEMORY_BACKEND}"
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --memhierarchy)
            MEMORY_BACKEND="memhierarchy"
            shift
            ;;
        --memory-backend)
            MEMORY_BACKEND="${2:?--memory-backend requires native or memhierarchy}"
            shift 2
            ;;
        --memory-backend=*)
            MEMORY_BACKEND="${1#*=}"
            shift
            ;;
        --*)
            echo "unknown option: $1" >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done
if [[ "${MEMORY_BACKEND}" != "native" &&
      "${MEMORY_BACKEND}" != "memhierarchy" ]]; then
    echo "memory backend must be native or memhierarchy" >&2
    exit 2
fi
readonly CASE_NAME="${1:?usage: run-test.sh [--memhierarchy] CASE_NAME}"
if [[ "$#" -ne 1 ]]; then
    echo "run-test.sh accepts exactly one model case" >&2
    exit 2
fi
for value in "${GOLEM_MODEL_MESH_ROWS}" "${GOLEM_MODEL_MESH_COLS}" \
    "${GOLEM_MODEL_DIGITAL_WORKERS}" "${GOLEM_MODEL_SST_THREADS}"; do
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
        echo "mesh dimensions, digital workers, and SST threads must be positive integers" >&2
        exit 2
    }
done
if [[ "${GOLEM_MODEL_BALANCE_DIGITAL_WORK}" != 0 &&
      "${GOLEM_MODEL_BALANCE_DIGITAL_WORK}" != 1 ]]; then
    echo "GOLEM_MODEL_BALANCE_DIGITAL_WORK must be 0 or 1" >&2
    exit 2
fi
case "${GOLEM_MODEL_MESH_ROUTER_BACKEND}" in
    merlin|mittens) ;;
    *)
        echo "GOLEM_MODEL_MESH_ROUTER_BACKEND must be merlin or mittens" >&2
        exit 2
        ;;
esac
case "${GOLEM_MODEL_SST_PARTITIONER}" in
    sst.simple|sst.linear|sst.roundrobin)
        ;;
    *)
        echo "GOLEM_MODEL_SST_PARTITIONER must be sst.simple, sst.linear, or sst.roundrobin" >&2
        exit 2
        ;;
esac
readonly MESH_TILE_COUNT=$((GOLEM_MODEL_MESH_ROWS * GOLEM_MODEL_MESH_COLS))
readonly RUN_TAG="mesh-${GOLEM_MODEL_MESH_ROWS}x${GOLEM_MODEL_MESH_COLS}-dw${GOLEM_MODEL_DIGITAL_WORKERS}-balance${GOLEM_MODEL_BALANCE_DIGITAL_WORK}-router${GOLEM_MODEL_MESH_ROUTER_BACKEND}-sst${GOLEM_MODEL_SST_THREADS}"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-ra-tree-model-suite/${MEMORY_BACKEND}/${CASE_NAME}/${RUN_TAG}"
readonly INPUT_MLIR="${OUTPUT_DIR}/model.mlir"
readonly DEPLOYMENT_DIR="${OUTPUT_DIR}/deployment"
readonly OBJECT_DIR="${OUTPUT_DIR}/cores"
readonly ELF="${OUTPUT_DIR}/tile-0.elf"
readonly LINKER_SCRIPT="${OUTPUT_DIR}/tile.ld"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly SIMULATION_LOG="${OUTPUT_DIR}/simulation.log"
readonly STATS="${OUTPUT_DIR}/router-statistics.csv"
readonly RESULT="${OUTPUT_DIR}/result.csv"
readonly TRACE_DIR="${OUTPUT_DIR}/trace"
readonly TILE_MAIN_SOURCE="${PROJECT_ROOT}/platform/sculptor-tile-main.cpp"

for executable in "${COMPILER_PYTHON}" "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" "${LLVM}/bin/llvm-readelf" "${QEMU}" "${SST}"; do
    require_executable "${executable}"
done
require_file "${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir/torch_mlir/fx.py"
require_file "${TILE_MAIN_SOURCE}"

read -r MODEL_ARRAY_ROWS MODEL_ARRAY_COLS < <(
    PYTHONPATH="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir" \
        "${COMPILER_PYTHON}" "${TEST_DIR}/import-model.py" \
        "${CASE_NAME}" ignored --print-hardware
)
readonly ARRAY_ROWS="${GOLEM_MODEL_ARRAY_ROWS:-${MODEL_ARRAY_ROWS}}"
readonly ARRAY_COLS="${GOLEM_MODEL_ARRAY_COLS:-${MODEL_ARRAY_COLS}}"
mkdir -p -- "${OUTPUT_DIR}"
PYTHONPATH="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir" \
TORCH_MLIR_OPT="${INSTALL_ROOT}/torch-mlir/bin/torch-mlir-opt" \
    "${COMPILER_PYTHON}" "${TEST_DIR}/import-model.py" \
    "${CASE_NAME}" "${INPUT_MLIR}"

"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
SCULPTOR_INPUT_MLIR="${INPUT_MLIR}" \
SCULPTOR_DEPLOYMENT_DIR="${DEPLOYMENT_DIR}" \
SCULPTOR_MESH_ROWS="${GOLEM_MODEL_MESH_ROWS}" \
SCULPTOR_MESH_COLS="${GOLEM_MODEL_MESH_COLS}" \
SCULPTOR_ARRAYS_PER_CORE="${GOLEM_MODEL_ARRAYS_PER_CORE}" \
SCULPTOR_ARRAY_ROWS="${ARRAY_ROWS}" SCULPTOR_ARRAY_COLS="${ARRAY_COLS}" \
SCULPTOR_DUPLICATE_MATRICES="${GOLEM_MODEL_DUPLICATE_MATRICES}" \
SCULPTOR_DIGITAL_WORKERS="${GOLEM_MODEL_DIGITAL_WORKERS}" \
SCULPTOR_BALANCE_DIGITAL_WORK="${GOLEM_MODEL_BALANCE_DIGITAL_WORK}" \
SCULPTOR_PLANNER_STRATEGIES="${GOLEM_MODEL_PLANNER_STRATEGIES}" \
    "${PROJECT_ROOT}/build-scripts/lower-sculptor-ra-tree.sh"

SCULPTOR_DEPLOYMENT_MLIR="${DEPLOYMENT_DIR}/09-tile-deployment.mlir" \
SCULPTOR_CORE_OBJECT_DIR="${OBJECT_DIR}" \
SCULPTOR_CORE_LTO=none \
    "${PROJECT_ROOT}/build-scripts/build-sculptor-core-objects.sh"

mapfile -t ACTIVE_CORES <"${OUTPUT_DIR}/active-cores.txt"
if [[ "${#ACTIVE_CORES[@]}" -eq 0 ]]; then
    echo "${CASE_NAME}: deployment did not generate active tiles" >&2
    exit 1
fi

common_flags=(
    "--target=${GOLEM_TARGET}" "-mcpu=${GOLEM_CPU}" "-mabi=${GOLEM_ABI}"
    -mcmodel=medany -ffreestanding -fno-stack-protector
    -ffunction-sections -fdata-sections -O2
)
cxx_flags=("${common_flags[@]}" -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
    -fno-asynchronous-unwind-tables -Wall -Wextra -Wpedantic -Werror
    "-I${RUNTIME_INCLUDE}" "-I${PROJECT_ROOT}/platform")
for source in crt0.S; do
    "${LLVM}/bin/clang" "${common_flags[@]}" -c "${PROJECT_ROOT}/platform/${source}" \
        -o "${OUTPUT_DIR}/${source}.o"
done
for source in uart platform-exit freestanding-memory freestanding-math; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" -c "${PROJECT_ROOT}/platform/${source}.cpp" \
        -o "${OUTPUT_DIR}/${source}.o"
done
"${LLVM}/bin/clang++" "${cxx_flags[@]}" -c "${TILE_MAIN_SOURCE}" \
    -o "${OUTPUT_DIR}/tile-main.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" -c "${TEST_DIR}/idle-main.cpp" \
    -o "${OUTPUT_DIR}/idle-main.o"
sed "s/__GOLEM_TILE_MEMORY__/${GOLEM_MODEL_TILE_MEMORY}/" \
    "${TEST_DIR}/tile-model-suite.ld" >"${LINKER_SCRIPT}"
link_tile() {
    local tile_id="$1"
    local main_object="$2"
    local generated_object="${3:-}"
    local elf="${OUTPUT_DIR}/tile-${tile_id}.elf"
    local -a objects=(
        "${OUTPUT_DIR}/crt0.S.o" "${OUTPUT_DIR}/uart.o"
        "${OUTPUT_DIR}/platform-exit.o" "${OUTPUT_DIR}/freestanding-memory.o"
        "${OUTPUT_DIR}/freestanding-math.o" "${main_object}"
    )
    [[ -z "${generated_object}" ]] || objects+=("${generated_object}" "${RUNTIME_LIBRARY}")
    "${LLVM}/bin/clang++" "${common_flags[@]}" -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${LINKER_SCRIPT}" "${objects[@]}" -o "${elf}"
    "${LLVM}/bin/llvm-readelf" -h "${elf}" | grep -q 'Machine:.*RISC-V'
}
for ((tile_id = 0; tile_id < MESH_TILE_COUNT; ++tile_id)); do
    object="${OBJECT_DIR}/core-${tile_id}.o"
    if [[ -s "${object}" ]]; then
        link_tile "${tile_id}" "${OUTPUT_DIR}/tile-main.o" "${object}"
    else
        link_tile "${tile_id}" "${OUTPUT_DIR}/idle-main.o"
    fi
done

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_SCULPTOR_ELF_DIRECTORY="${OUTPUT_DIR}"
export MITTENS_SCULPTOR_ACTIVE_CORES="$(IFS=,; echo "${ACTIVE_CORES[*]}")"
export MITTENS_SCULPTOR_STATS="${STATS}"
export MITTENS_SCULPTOR_MESH_ROWS="${GOLEM_MODEL_MESH_ROWS}"
export MITTENS_SCULPTOR_MESH_COLS="${GOLEM_MODEL_MESH_COLS}"
export MITTENS_ANALOG_ARRAY_COUNT="${GOLEM_MODEL_ARRAYS_PER_CORE}"
export MITTENS_ANALOG_ARRAY_ROWS="${ARRAY_ROWS}"
export MITTENS_ANALOG_ARRAY_COLUMNS="${ARRAY_COLS}"
export MITTENS_SCULPTOR_MEMORY_BACKEND="${MEMORY_BACKEND}"
export MITTENS_SCULPTOR_TILE_MEMORY="${GOLEM_MODEL_TILE_MEMORY}"
export MITTENS_SCULPTOR_CPU_CLOCK="${GOLEM_MODEL_CPU_CLOCK}"
export MITTENS_SCULPTOR_CPU_ISSUE_WIDTH="${GOLEM_MODEL_CPU_ISSUE_WIDTH}"
export MITTENS_SCULPTOR_SYNC_INSTRUCTION_QUANTUM="${GOLEM_MODEL_SYNC_INSTRUCTION_QUANTUM}"
export MITTENS_SCULPTOR_ANALOG_BACKEND="${GOLEM_MODEL_ANALOG_BACKEND}"
export MITTENS_SCULPTOR_ANALOG_LINK_CLOCK="${GOLEM_MODEL_ANALOG_LINK_CLOCK}"
export MITTENS_SCULPTOR_ANALOG_COMPUTE_LATENCY_CYCLES="${GOLEM_MODEL_ANALOG_COMPUTE_LATENCY_CYCLES}"
export MITTENS_SCULPTOR_MESH_LINK_WIDTH_BITS="${GOLEM_MODEL_MESH_LINK_WIDTH_BITS}"
export MITTENS_SCULPTOR_MESH_LINK_CLOCK="${GOLEM_MODEL_MESH_LINK_CLOCK}"
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
if [[ "${GOLEM_MODEL_PROFILE_MODE}" == trace ]]; then
    export MITTENS_SCULPTOR_TASK_TRACE_DIRECTORY="${TRACE_DIR}/tasks"
else
    export MITTENS_SCULPTOR_TASK_TRACE_DIRECTORY=""
fi
export MITTENS_SCULPTOR_MEMORY_INIT_BATCHING="${GOLEM_MODEL_MEMORY_INIT_BATCHING}"
export MITTENS_SCULPTOR_MEMORY_ACCESS_BATCHING="${GOLEM_MODEL_MEMORY_ACCESS_BATCHING}"
export MITTENS_SCULPTOR_MEMORY_ACCESS_BATCH_RECORDS="${GOLEM_MODEL_MEMORY_ACCESS_BATCH_RECORDS}"
export MITTENS_SCULPTOR_MEMORY_LOAD_QUEUE_ENTRIES="${GOLEM_MODEL_MEMORY_LOAD_QUEUE_ENTRIES:-8}"
export MITTENS_SCULPTOR_MEMORY_STORE_BUFFER_ENTRIES="${GOLEM_MODEL_MEMORY_STORE_BUFFER_ENTRIES:-8}"
export MITTENS_SCULPTOR_MEMORY_INIT_BYTES_PER_CYCLE="${GOLEM_MODEL_MEMORY_INIT_BYTES_PER_CYCLE}"
export MITTENS_SCULPTOR_MEMORY_INIT_LATENCY_CYCLES="${GOLEM_MODEL_MEMORY_INIT_LATENCY_CYCLES}"
export MITTENS_SCULPTOR_L1_SIZE="${GOLEM_MODEL_L1_SIZE}"
export MITTENS_SCULPTOR_L1_ASSOCIATIVITY="${GOLEM_MODEL_L1_ASSOCIATIVITY}"
export MITTENS_SCULPTOR_CACHE_LINE_SIZE="${GOLEM_MODEL_CACHE_LINE_SIZE}"
export MITTENS_SCULPTOR_L1_ACCESS_LATENCY_CYCLES="${GOLEM_MODEL_L1_ACCESS_LATENCY_CYCLES}"
export MITTENS_SCULPTOR_L1_MAX_REQUESTS_PER_CYCLE="${GOLEM_MODEL_L1_MAX_REQUESTS_PER_CYCLE:-1}"
export MITTENS_SCULPTOR_L1_BANKS="${GOLEM_MODEL_L1_BANKS:-1}"
export MITTENS_SCULPTOR_L1_CLOCK="${GOLEM_MODEL_L1_CLOCK}"
export MITTENS_SCULPTOR_LOWER_MEMORY_CLOCK="${GOLEM_MODEL_LOWER_MEMORY_CLOCK}"
export MITTENS_SCULPTOR_LOWER_MEMORY_ACCESS_TIME="${GOLEM_MODEL_LOWER_MEMORY_ACCESS_TIME}"
rm -f -- "${SIMULATION_LOG}" "${STATS}" "${RESULT}"
simulation_start_ns="$(date +%s%N)"
"${SST}" -n "${GOLEM_MODEL_SST_THREADS}" \
    --partitioner="${GOLEM_MODEL_SST_PARTITIONER}" \
    "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_LOG}"
simulation_end_ns="$(date +%s%N)"
simulation_wall_seconds="$(awk -v start="${simulation_start_ns}" \
    -v end="${simulation_end_ns}" 'BEGIN { printf "%.6f", (end - start) / 1000000000 }')"
if [[ "$(grep -Fc 'SCULPTOR_RA_SIM_PASS' "${SIMULATION_LOG}")" -ne "${#ACTIVE_CORES[@]}" ]]; then
    echo "${CASE_NAME}: not every active tile completed" >&2
    exit 1
fi
simulated_time="$(awk '/Simulation is complete/ { print $(NF - 1) " " $NF }' "${SIMULATION_LOG}" | tail -n1)"
if [[ -z "${simulated_time}" ]]; then
    echo "${CASE_NAME}: SST did not report completion time" >&2
    exit 1
fi
printf 'case,mesh,digital_workers,balance_digital_work,router_backend,active_tiles,sst_threads,sst_partitioner,simulated_time,wall_seconds\n%s,%sx%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "${CASE_NAME}" "${GOLEM_MODEL_MESH_ROWS}" "${GOLEM_MODEL_MESH_COLS}" \
    "${GOLEM_MODEL_DIGITAL_WORKERS}" "${GOLEM_MODEL_BALANCE_DIGITAL_WORK}" \
    "${GOLEM_MODEL_MESH_ROUTER_BACKEND}" \
    "${#ACTIVE_CORES[@]}" "${GOLEM_MODEL_SST_THREADS}" \
    "${GOLEM_MODEL_SST_PARTITIONER}" "${simulated_time}" \
    "${simulation_wall_seconds}" >"${RESULT}"
echo "${CASE_NAME} [${MEMORY_BACKEND}, ${RUN_TAG}]: PyTorch -> RA-tree tile ELF -> QEMU + SST: PASS (${simulated_time}, ${simulation_wall_seconds}s wall)"
