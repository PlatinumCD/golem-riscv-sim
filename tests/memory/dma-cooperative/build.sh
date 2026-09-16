#!/usr/bin/env bash
set -euo pipefail
TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../support/test-env.sh"
OUTPUT="${1:?output directory}"
mkdir -p "$OUTPUT"
LLVM="${GOLEM_LLVM_DIR:-${PROJECT_ROOT}/install/llvm}"
common=("--target=${GOLEM_TARGET}" "-mcpu=${GOLEM_CPU}" "-mabi=${GOLEM_ABI}" -mcmodel=medany -ffreestanding -fno-stack-protector -ffunction-sections -fdata-sections -O3 -g)
cxx=("${common[@]}" -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-unwind-tables -fno-asynchronous-unwind-tables "-I${PLATFORM_ROOT}")
"${LLVM}/bin/clang" "${common[@]}" -c "${PLATFORM_STARTUP_ROOT}/crt0.S" -o "$OUTPUT/crt0.o"
for source in "${PLATFORM_ROOT}/uart.cpp" "${PLATFORM_STARTUP_ROOT}/platform-exit.cpp"; do
    "${LLVM}/bin/clang++" "${cxx[@]}" -c "$source" -o "$OUTPUT/$(basename "$source").o"
done
for mode in 0 1 2 3; do
    for tile in 0 1; do
        extra=("-DMESH_ROUNDS=${DMA_QUERY_ROUNDS:-64}" "-DWORK_ITERATIONS=${DMA_QUERY_WORK:-0}" "-DDETAILED_TRACE=${DMA_QUERY_DETAILED:-0}")
        if [[ -n "${DMA_QUERY_INTERVAL:-}" ]]; then extra+=("-DPOLL_INTERVAL=$DMA_QUERY_INTERVAL"); fi
        "${LLVM}/bin/clang++" "${cxx[@]}" "${extra[@]}" -DTILE="$tile" -DCOOPERATIVE="$mode" -c "$TEST_DIR/main.cpp" -o "$OUTPUT/main.o"
        "${LLVM}/bin/clang++" "${common[@]}" -nostdlib -fuse-ld=lld -Wl,--gc-sections -Wl,--defsym,SPM_CODE_OFFSET=131072 "-Wl,-T,${PLATFORM_STARTUP_ROOT}/scratchpad.ld" "$OUTPUT/crt0.o" "$OUTPUT/uart.cpp.o" "$OUTPUT/platform-exit.cpp.o" "$OUTPUT/main.o" -o "$OUTPUT/m${mode}-t${tile}.elf"
        "${LLVM}/bin/llvm-objdump" -d --demangle "$OUTPUT/m${mode}-t${tile}.elf" > "$OUTPUT/m${mode}-t${tile}.asm"
    done
done
