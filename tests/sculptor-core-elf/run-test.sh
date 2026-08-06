#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly MVM_EXECUTION="${SCULPTOR_CORE_MVM_EXECUTION:-analog}"
readonly SOURCE_PIPELINE_TEST="${PROJECT_ROOT}/tests/torch-mlir-sculptor/run-test.sh"
readonly ANALOG_SOURCE_MLIR="${BUILD_ROOT}/tests/torch-mlir-sculptor/two-linear-llvm-shims.mlir"
readonly SCHEDULED_MLIR="${BUILD_ROOT}/tests/torch-mlir-sculptor/two-linear-scheduled.mlir"
case "${MVM_EXECUTION}" in
    analog)
        readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-core-elf"
        readonly SOURCE_MLIR="${ANALOG_SOURCE_MLIR}"
        ;;
    digital)
        readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-core-elf-digital"
        readonly SOURCE_MLIR="${OUTPUT_DIR}/two-linear-digital-llvm-shims.mlir"
        ;;
    *)
        echo "SCULPTOR_CORE_MVM_EXECUTION must be analog or digital" >&2
        exit 1
        ;;
esac
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly STATISTICS="${OUTPUT_DIR}/router-statistics.csv"
readonly SIMULATION_OUTPUT="${OUTPUT_DIR}/simulation.log"
for executable in \
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

"${SOURCE_PIPELINE_TEST}"
"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
require_file "${RUNTIME_LIBRARY}"
mkdir -p -- "${OUTPUT_DIR}"

if [[ "${MVM_EXECUTION}" == digital ]]; then
    require_file "${SCHEDULED_MLIR}"
    "${SCULPTOR_OPT}" "${SCHEDULED_MLIR}" \
        --sculptor-lower-scheduled-mvm-to-digital \
        --sculptor-fuse-task-graph \
        --sculptor-lower-golem-to-llvm-shims \
        -o "${SOURCE_MLIR}"

    if [[ "$(grep -c 'linalg.matmul_transpose_b' "${SOURCE_MLIR}")" -ne 2 ]] ||
       grep -Eq 'sculptor\.array\.|golem_analog_mvm_(set|load|compute|store)' \
        "${SOURCE_MLIR}"; then
        echo "digital MVM lowering did not produce two pure digital matmuls" >&2
        exit 1
    fi
fi
require_file "${SOURCE_MLIR}"

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
    -c "${TEST_DIR}/freestanding-memory.cpp" \
    -o "${OUTPUT_DIR}/freestanding-memory.o"

build_core() {
    local core_id="$1"
    local prefix="${OUTPUT_DIR}/core-${core_id}"
    local elf="${prefix}.elf"
    local entry

    if [[ "${core_id}" != 0 && "${core_id}" != 1 ]]; then
        echo "unsupported deployment core ${core_id}" >&2
        return 1
    fi

    "${SCULPTOR_OPT}" "${SOURCE_MLIR}" \
        --sculptor-partition-task-graph-by-core \
        --sculptor-extract-core-module="core-id=${core_id}" \
        --sculptor-finalize-task-graph-resources \
        -o "${prefix}-isolated.mlir"

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
        -o "${prefix}-task-only.mlir"

    "${LLVM}/bin/mlir-translate" \
        --allow-unregistered-dialect \
        --mlir-to-llvmir \
        "${prefix}-task-only.mlir" \
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
    if [[ "${entry}" != "0x80000000" ]]; then
        echo "unexpected core-${core_id} ELF entry point: ${entry}" >&2
        return 1
    fi
    if "${LLVM}/bin/llvm-readelf" --program-headers "${elf}" |
        grep -q INTERP; then
        echo "core-${core_id} ELF unexpectedly contains an interpreter" >&2
        return 1
    fi

    if [[ "${MVM_EXECUTION}" == analog ]]; then
        if [[ "$("${LLVM}/bin/llvm-objdump" -d "${elf}" |
            grep -c '<unknown>')" -lt 4 ]]; then
            echo "core-${core_id} ELF lacks the four Golem analog opcodes" >&2
            return 1
        fi
    elif "${LLVM}/bin/llvm-objdump" -d "${elf}" | grep -q '<unknown>'; then
        echo "core-${core_id} digital ELF unexpectedly contains a custom opcode" >&2
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
            echo "core-${core_id} ELF is missing generated ABI ${symbol}" >&2
            return 1
        fi
    done

    echo "built generated Sculptor core ${core_id}: ${elf}"
}

build_core 0
build_core 1

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_SCULPTOR_CORE0_ELF="${OUTPUT_DIR}/core-0.elf"
export MITTENS_SCULPTOR_CORE1_ELF="${OUTPUT_DIR}/core-1.elf"
export MITTENS_SCULPTOR_STATS="${STATISTICS}"

rm -f -- "${STATISTICS}" "${SIMULATION_OUTPUT}"
"${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"

grep -Fq "SCULPTOR_BASIC_RUNTIME_CORE_0_PASS" "${SIMULATION_OUTPUT}"
grep -Fq "SCULPTOR_BASIC_RUNTIME_CORE_1_PASS" "${SIMULATION_OUTPUT}"
grep -Fq \
    "[core 0] task 1 produced [2,4,6] and routed 3 words" \
    "${SIMULATION_OUTPUT}"
grep -Fq \
    "[core 1] task 3 produced model output [12,4]" \
    "${SIMULATION_OUTPUT}"

if [[ ! -s "${STATISTICS}" ]]; then
    echo "the generated runtime proof did not produce router statistics" >&2
    exit 1
fi
if ! awk -F, '
    $1 == "router_0_0" &&
    $2 == "send_packet_count" &&
    $3 == "port0" {
        activation_words += $7
    }
    $1 == "router_1_0" &&
    $2 == "send_packet_count" &&
    $3 == "port1" {
        control_words += $7
    }
    $2 == "send_bit_count" &&
    $3 == "port4" {
        ejected_bits += $7
        if ($10 != 32 || $11 != 32) {
            invalid_width = 1
        }
    }
    END {
        valid = activation_words == 3 &&
                control_words == 2 &&
                ejected_bits == 160 &&
                !invalid_width
        exit !valid
    }
' "${STATISTICS}"; then
    echo "expected three activation words and two 32-bit control words" >&2
    exit 1
fi

echo "PyTorch -> ${MVM_EXECUTION} MVM -> two generated core ELFs -> basic runtime -> [12,4]: PASS"
