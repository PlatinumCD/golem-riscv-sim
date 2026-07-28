#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly COMPILER_ARTIFACTS="${RESNET18_COMPILER_ARTIFACTS:-${BUILD_ROOT}/tests/sculptor-resnet18-8x8/compiler-3ed3bdb/cores}"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-resnet18-8x8/deployment"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly LINKER_SCRIPT="${TEST_DIR}/tile-64m.ld"
readonly CODEGEN_OPT_LEVEL="${MITTENS_RESNET18_OPT_LEVEL:--O3}"
readonly CODEGEN_LTO="${MITTENS_RESNET18_LTO:-full}"
readonly RUNTIME_PROFILE="${MITTENS_RESNET18_RUNTIME_PROFILE:-0}"
readonly TASK_TRACE="${MITTENS_RESNET18_TASK_TRACE:-0}"

case "${CODEGEN_OPT_LEVEL}" in
    -O0|-O1|-O2|-O3|-Os|-Oz) ;;
    *)
        echo "unsupported MITTENS_RESNET18_OPT_LEVEL: ${CODEGEN_OPT_LEVEL}" >&2
        exit 2
        ;;
esac
case "${CODEGEN_LTO}" in
    none|thin|full) ;;
    *)
        echo "MITTENS_RESNET18_LTO must be none, thin, or full" >&2
        exit 2
        ;;
esac
case "${RUNTIME_PROFILE}" in
    0|1) ;;
    *)
        echo "MITTENS_RESNET18_RUNTIME_PROFILE must be 0 or 1" >&2
        exit 2
        ;;
esac
case "${TASK_TRACE}" in
    0|1) ;;
    *)
        echo "MITTENS_RESNET18_TASK_TRACE must be 0 or 1" >&2
        exit 2
        ;;
esac

optimization_flags=("${CODEGEN_OPT_LEVEL}")
lto_flags=()
profile_flags=()
if [[ "${CODEGEN_LTO}" != "none" ]]; then
    optimization_flags+=("-flto=${CODEGEN_LTO}")
    lto_flags+=("-flto=${CODEGEN_LTO}")
fi
if [[ "${RUNTIME_PROFILE}" == "1" ]]; then
    profile_flags+=(
        "-DGOLEM_RUNTIME_ENABLE_PROFILE=1"
        "-DMITTENS_RESNET18_RUNTIME_PROFILE=1"
    )
fi
if [[ "${TASK_TRACE}" == "1" ]]; then
    profile_flags+=("-DMITTENS_RESNET18_TASK_TRACE=1")
fi

for executable in \
    "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" \
    "${LLVM}/bin/llvm-nm" \
    "${LLVM}/bin/llvm-readelf"; do
    require_executable "${executable}"
done
for core_id in {0..18}; do
    require_file "${COMPILER_ARTIFACTS}/core-${core_id}.o"
done

GOLEM_CODEGEN_OPT_LEVEL="${CODEGEN_OPT_LEVEL}" \
GOLEM_CODEGEN_LTO="${CODEGEN_LTO}" \
GOLEM_RUNTIME_ENABLE_PROFILE="${RUNTIME_PROFILE}" \
    "${PROJECT_ROOT}/build-scripts/build-runtime.sh"
mkdir -p -- "${OUTPUT_DIR}"

common_flags=(
    "--target=${GOLEM_TARGET}"
    "-mcpu=${GOLEM_CPU}"
    "-mabi=${GOLEM_ABI}"
    -mcmodel=medany
    -ffreestanding
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    "${optimization_flags[@]}"
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
    "${profile_flags[@]}"
)

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
for source in \
    uart \
    platform-exit \
    freestanding-memory \
    mlir-runtime; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/${source}.cpp" \
        -o "${OUTPUT_DIR}/${source}.o"
done
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/tile-main.cpp" \
    -o "${OUTPUT_DIR}/tile-main.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/idle-main.cpp" \
    -o "${OUTPUT_DIR}/idle-main.o"

link_elf() {
    local output="$1"
    shift
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        "${lto_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${LINKER_SCRIPT}" \
        "$@" \
        -o "${output}"
}

echo "ResNet-18 tile code generation: ${CODEGEN_OPT_LEVEL}, LTO=${CODEGEN_LTO}"
echo "ResNet-18 runtime cycle profiling: ${RUNTIME_PROFILE}"
echo "ResNet-18 task tracing: ${TASK_TRACE}"

link_elf \
    "${OUTPUT_DIR}/idle.elf" \
    "${OUTPUT_DIR}/crt0.o" \
    "${OUTPUT_DIR}/platform-exit.o" \
    "${OUTPUT_DIR}/idle-main.o"

for core_id in {0..18}; do
    elf="${OUTPUT_DIR}/core-${core_id}.elf"
    link_elf \
        "${elf}" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${OUTPUT_DIR}/mlir-runtime.o" \
        "${OUTPUT_DIR}/tile-main.o" \
        "${COMPILER_ARTIFACTS}/core-${core_id}.o" \
        "${RUNTIME_LIBRARY}"

    entry="$("${LLVM}/bin/llvm-readelf" --file-header "${elf}" |
        awk '/Entry point address:/ {print $4}')"
    if [[ "${entry}" != "0x80000000" ]]; then
        echo "core ${core_id} has unexpected entry point ${entry}" >&2
        exit 1
    fi
    if "${LLVM}/bin/llvm-nm" --undefined-only \
        --format=just-symbols "${elf}" | grep '.' >/dev/null; then
        echo "core ${core_id} has undefined symbols" >&2
        "${LLVM}/bin/llvm-nm" --undefined-only "${elf}" >&2
        exit 1
    fi
    echo "linked ResNet-18 deployment core ${core_id}: ${elf}"
done

echo "linked 19 active ResNet-18 tile ELFs and one idle ELF"
