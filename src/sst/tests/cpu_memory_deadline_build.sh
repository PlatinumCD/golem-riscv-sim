#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../../tests/support/test-env.sh"
readonly OUTPUT="${1:?output directory}"
readonly LLVM="${GOLEM_LLVM_DIR:-${PROJECT_ROOT}/install/llvm}"
"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
mkdir -p "${OUTPUT}"
common=("--target=${GOLEM_TARGET}" "-mcpu=${GOLEM_CPU}" "-mabi=${GOLEM_ABI}"
    -mcmodel=medany -ffreestanding -fno-stack-protector
    -ffunction-sections -fdata-sections -O2 -g)
cxx=("${common[@]}" -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
    -fno-asynchronous-unwind-tables -Wall -Wextra -Werror "-I${PLATFORM_ROOT}")
"${LLVM}/bin/clang" "${common[@]}" -c "${PLATFORM_STARTUP_ROOT}/crt0.S" -o "${OUTPUT}/crt0.o"
"${LLVM}/bin/clang++" "${cxx[@]}" -c "${PLATFORM_ROOT}/uart.cpp" -o "${OUTPUT}/uart.o"
"${LLVM}/bin/clang++" "${cxx[@]}" -c "${PLATFORM_STARTUP_ROOT}/platform-exit.cpp" -o "${OUTPUT}/exit.o"
for mode in delivery spm; do
    spm=0
    [[ "${mode}" != spm ]] || spm=1
    "${LLVM}/bin/clang++" "${cxx[@]}" "-DDEADLINE_SPM=${spm}" \
        -c "${TEST_DIR}/cpu_memory_deadline.cpp" -o "${OUTPUT}/${mode}.o"
    "${LLVM}/bin/clang++" "${common[@]}" -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT}/crt0.o" "${OUTPUT}/uart.o" "${OUTPUT}/exit.o" "${OUTPUT}/${mode}.o" \
        "${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a" -o "${OUTPUT}/${mode}.elf"
done
