#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly MODEL_TEST_DIR="${PROJECT_ROOT}/tests/sculptor-eight-layer-mesh"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-eight-layer-mesh-4x2"
readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly LINALG_MLIR="${OUTPUT_DIR}/eight-linear-linalg.mlir"
readonly ISLANDS_MLIR="${OUTPUT_DIR}/eight-linear-islands.mlir"
readonly SCHEDULED_MLIR="${OUTPUT_DIR}/eight-linear-scheduled.mlir"
readonly FUSED_MLIR="${OUTPUT_DIR}/eight-linear-fused.mlir"
readonly LLVM_SHIMS_MLIR="${OUTPUT_DIR}/eight-linear-llvm-shims.mlir"
readonly STATISTICS="${OUTPUT_DIR}/router-statistics.csv"
readonly SIMULATION_OUTPUT="${OUTPUT_DIR}/simulation.log"

for executable in \
    "${COMPILER_PYTHON}" \
    "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" \
    "${LLVM}/bin/mlir-translate" \
    "${LLVM}/bin/llvm-nm" \
    "${LLVM}/bin/llvm-objdump" \
    "${LLVM}/bin/llvm-readelf" \
    "${SCULPTOR_OPT}" \
    "${QEMU}" \
    "${SST}"; do
    require_executable "${executable}"
done
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"
require_file "${MODEL_TEST_DIR}/import-model.py"
require_file "${MODEL_TEST_DIR}/freestanding-memory.cpp"
mkdir -p -- "${OUTPUT_DIR}"

PYTHONPATH="${TORCH_MLIR_PYTHON}" \
    "${COMPILER_PYTHON}" \
    "${MODEL_TEST_DIR}/import-model.py" \
    "${LINALG_MLIR}"

if [[ "$(grep -c 'linalg.matmul' "${LINALG_MLIR}")" -ne 8 ]]; then
    echo "Torch-MLIR did not emit eight linear matmuls" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${LINALG_MLIR}" \
    --sculptor-canonicalize-layers \
    --sculptor-extract-layers \
    --sculptor-convert-layers \
    --sculptor-expand-mvm-to-golem="array-rows=8 array-cols=8" \
    --sculptor-materialize-tasks \
    --sculptor-assemble-task-graph \
    --sculptor-build-task-graph-islands \
    -o "${ISLANDS_MLIR}"

"${SCULPTOR_OPT}" "${ISLANDS_MLIR}" \
    --sculptor-schedule-task-graph="cores=8 arrays-per-core=1 topology=mesh mesh-rows=2 mesh-cols=4 schedule=snake" \
    -o "${SCHEDULED_MLIR}"

declare -a expected_cores=(0 1 2 3 7 6 5 4)
for layer_id in {0..7}; do
    layer="linearwbias_${layer_id}"
    core="${expected_cores[${layer_id}]}"

    if [[ "$(grep -c \
        "sculptor.task.create.*source_layer = \"${layer}\".*sculptor.runtime.core_id = ${core} : i64" \
        "${SCHEDULED_MLIR}")" -ne 5 ]] ||
       [[ "$(grep -c \
        "sculptor.task.create.*source_layer = \"${layer}\".*sculptor.runtime.local_array_id = 0 : i64.*sculptor.runtime.physical_array_id = ${core} : i64" \
        "${SCHEDULED_MLIR}")" -ne 2 ]]; then
        echo "${layer} has unexpected 4x2 core or analog-array placement" >&2
        exit 1
    fi
done
if ! grep -Fq \
    'sculptor.schedule.logical_array_to_analog_array = [0, 1, 2, 3, 7, 6, 5, 4]' \
    "${SCHEDULED_MLIR}" ||
   ! grep -Fq \
    'sculptor.schedule.inter_core_transfer_bytes = 112 : i64' \
    "${SCHEDULED_MLIR}"; then
    echo "the eight-layer 4x2 snake schedule has unexpected metadata" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${SCHEDULED_MLIR}" \
    --sculptor-fuse-task-graph \
    -o "${FUSED_MLIR}"

if [[ "$(grep -c 'sculptor.task.create' "${FUSED_MLIR}")" -ne 16 ]] ||
   [[ "$(grep 'sculptor.task.create' "${FUSED_MLIR}" |
        grep -c 'task_kind = \"sculptor.matrix_setup\"')" -ne 8 ]] ||
   [[ "$(grep 'sculptor.task.create' "${FUSED_MLIR}" |
        grep -c 'task_kind = \"mixed.fused\"')" -ne 8 ]]; then
    echo "Sculptor did not fuse the graph to eight setup and eight compute tasks" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${FUSED_MLIR}" \
    --sculptor-lower-golem-to-llvm-shims \
    -o "${LLVM_SHIMS_MLIR}"

for shim in set load compute store; do
    if [[ "$(grep -c "call @golem_analog_mvm_${shim}" \
        "${LLVM_SHIMS_MLIR}")" -ne 8 ]]; then
        echo "expected eight ${shim} shim calls" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
require_file "${RUNTIME_LIBRARY}"

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
    "-I${RUNTIME_INCLUDE}"
    "-I${PROJECT_ROOT}/platform"
)

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/uart.cpp" \
    -o "${OUTPUT_DIR}/uart.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/platform-exit.cpp" \
    -o "${OUTPUT_DIR}/platform-exit.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${MODEL_TEST_DIR}/freestanding-memory.cpp" \
    -o "${OUTPUT_DIR}/freestanding-memory.o"

build_core() {
    local core_id="$1"
    local prefix="${OUTPUT_DIR}/core-${core_id}"
    local elf="${prefix}.elf"
    local entry

    "${SCULPTOR_OPT}" "${LLVM_SHIMS_MLIR}" \
        --sculptor-partition-task-graph-by-core \
        --sculptor-extract-core-module="core-id=${core_id}" \
        --sculptor-finalize-task-graph-resources \
        -o "${prefix}-isolated.mlir"

    if [[ "$(grep -c 'sculptor.task.create' \
        "${prefix}-isolated.mlir")" -ne 2 ]] ||
       [[ "$(grep 'sculptor.task.create' "${prefix}-isolated.mlir" |
        grep -c 'task_kind = \"sculptor.matrix_setup\"')" -ne 1 ]] ||
       [[ "$(grep 'sculptor.task.create' "${prefix}-isolated.mlir" |
        grep -c 'task_kind = \"mixed.fused\"')" -ne 1 ]]; then
        echo "core ${core_id} does not contain one setup and one compute task" >&2
        return 1
    fi

    "${SCULPTOR_OPT}" "${prefix}-isolated.mlir" \
        --canonicalize \
        --cse \
        --empty-tensor-to-alloc-tensor \
        --one-shot-bufferize="bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map" \
        --convert-bufferization-to-memref \
        --convert-linalg-to-loops \
        --lower-affine \
        --convert-scf-to-cf \
        --convert-math-to-llvm \
        --expand-strided-metadata \
        --lower-affine \
        --convert-arith-to-llvm \
        --convert-index-to-llvm \
        --convert-cf-to-llvm \
        --finalize-memref-to-llvm \
        --convert-func-to-llvm \
        --reconcile-unrealized-casts \
        -o "${prefix}-compute-llvm.mlir"

    "${SCULPTOR_OPT}" "${prefix}-compute-llvm.mlir" \
        --sculptor-emit-golem-tile-abi \
        --sculptor-finalize-golem-intrinsics \
        -o "${prefix}-tile-abi.mlir"

    "${LLVM}/bin/mlir-translate" \
        --allow-unregistered-dialect \
        --mlir-to-llvmir \
        "${prefix}-tile-abi.mlir" \
        -o "${prefix}.ll"
    "${LLVM}/bin/clang" "${common_flags[@]}" \
        -Wno-override-module \
        -c "${prefix}.ll" \
        -o "${prefix}.o"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DGOLEM_DEPLOYMENT_CORE_ID=${core_id}" \
        -c "${TEST_DIR}/main.cpp" \
        -o "${prefix}-main.o"

    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
        "-Wl,-Map,${prefix}.map" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${prefix}-main.o" \
        "${prefix}.o" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"

    entry="$("${LLVM}/bin/llvm-readelf" --file-header "${elf}" |
        awk '/Entry point address:/ { print $4 }')"
    if [[ "${entry}" != "0x80000000" ]] ||
       "${LLVM}/bin/llvm-readelf" --program-headers "${elf}" |
        grep -q INTERP; then
        echo "core ${core_id} is not a freestanding tile ELF" >&2
        return 1
    fi
    if [[ "$("${LLVM}/bin/llvm-objdump" -d "${elf}" |
        grep -c '<unknown>')" -lt 4 ]]; then
        echo "core ${core_id} lacks its four analog operations" >&2
        return 1
    fi
    for symbol in \
        golem_tile_core_id \
        golem_tile_boot_tasks \
        golem_tile_dispatch_tasks \
        golem_tile_incoming_routes \
        golem_tile_outgoing_routes; do
        if ! "${LLVM}/bin/llvm-nm" "${elf}" |
            awk '{ print $3 }' |
            grep -Fxq "${symbol}"; then
            echo "core ${core_id} is missing generated ABI ${symbol}" >&2
            return 1
        fi
    done
    echo "built generated core ${core_id}: ${elf}"
}

for core_id in {0..7}; do
    build_core "${core_id}"
done

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_SCULPTOR_EIGHT_LAYER_4X2_STATS="${STATISTICS}"
for core_id in {0..7}; do
    export "MITTENS_SCULPTOR_EIGHT_LAYER_4X2_CORE${core_id}_ELF=${OUTPUT_DIR}/core-${core_id}.elf"
done

rm -f -- "${STATISTICS}" "${SIMULATION_OUTPUT}"
"${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"

grep -Fq \
    "[core 0] all eight tiles ready; dispatching model input" \
    "${SIMULATION_OUTPUT}"
grep -Fq \
    "[core 4] final model output [56,42,42,60]" \
    "${SIMULATION_OUTPUT}"
grep -Fq "SCULPTOR_EIGHT_LAYER_4X2_PASS" "${SIMULATION_OUTPUT}"

if [[ ! -s "${STATISTICS}" ]]; then
    echo "the eight-layer 4x2 proof did not produce router statistics" >&2
    exit 1
fi
if ! awk -F, '
    $1 ~ /^router_/ &&
    $2 == "send_bit_count" &&
    $3 == "port4" {
        ejected_bits += $7
        if ($7 != 0 && ($10 != 32 || $11 != 32)) {
            invalid_width = 1
        }
    }
    $1 ~ /^router_/ &&
    $2 == "send_bit_count" &&
    $3 != "port4" {
        physical_link_bits += $7
        if ($7 != 0 && ($10 != 32 || $11 != 32)) {
            invalid_width = 1
        }
    }
    END {
        valid = ejected_bits == 1152 &&
                physical_link_bits == 1440 &&
                !invalid_width
        exit !valid
    }
' "${STATISTICS}"; then
    echo "unexpected 4x2 route traffic or non-32-bit transfer" >&2
    exit 1
fi

completion_time="$(awk \
    '/Simulation is complete/ { print $(NF - 1), $NF }' \
    "${SIMULATION_OUTPUT}")"
echo "eight-layer PyTorch -> eight generated ELFs -> 4x2 mesh: PASS"
echo "simulated completion time: ${completion_time}"
