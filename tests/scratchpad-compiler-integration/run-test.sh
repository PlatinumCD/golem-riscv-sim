#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../build-scripts/common.sh"
readonly OUT="${BUILD_ROOT}/tests/scratchpad-compiler-integration"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly RUNTIME="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly ELF="${OUT}/scratchpad-compiler-integration.elf"

mkdir -p "${OUT}"
"${PROJECT_ROOT}/build-scripts/build-platform.sh" hello
"${OPT}" "${TEST_DIR}/input.mlir" \
    --sculptor-emit-golem-tile-abi \
    --sculptor-finalize-golem-intrinsics \
    -o "${OUT}/abi.mlir"
"${LLVM}/bin/mlir-translate" --mlir-to-llvmir \
    "${OUT}/abi.mlir" -o "${OUT}/abi.ll"

flags=(--target="${GOLEM_TARGET}" -mcpu="${GOLEM_CPU}" -mabi="${GOLEM_ABI}"
    -mcmodel=medany -ffreestanding -fno-stack-protector -O2)
cxx_flags=("${flags[@]}" -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
    -fno-asynchronous-unwind-tables -I"${INSTALL_ROOT}/runtime/include"
    -I"${PROJECT_ROOT}/platform")
"${LLVM}/bin/clang" "${flags[@]}" -Wno-override-module \
    -c "${OUT}/abi.ll" -o "${OUT}/abi.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/main.cpp" -o "${OUT}/main.o"
"${LLVM}/bin/clang++" "${flags[@]}" -nostdlib -nostartfiles -nodefaultlibs \
    -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
    -Wl,-T,"${PROJECT_ROOT}/platform/tile.ld" \
    "${BUILD_ROOT}/tests/hello/crt0.o" \
    "${BUILD_ROOT}/tests/hello/uart.o" \
    "${BUILD_ROOT}/tests/hello/platform-exit.o" \
    "${OUT}/main.o" "${OUT}/abi.o" "${RUNTIME}" -o "${ELF}"

export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_ELF="${ELF}"
"${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${OUT}/output.txt"
grep -F "compiler scratchpad integration: PASS" "${OUT}/output.txt"
