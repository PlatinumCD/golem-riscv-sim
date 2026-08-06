#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../build-scripts/common.sh
source "${TEST_DIR}/../../build-scripts/common.sh"

readonly OUT="${BUILD_ROOT}/tests/sculptor-gpt2-c3-isolated"
readonly BASELINE_CORES="${BUILD_ROOT}/tests/sculptor-gpt2-factorial-ab-hoist-static-allocs/tokens-4/configurations/br0-cr0-lp0-rb0-dm0/cores"
readonly CANDIDATE_LL="/tmp/gpt2-c3-core0.ll"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly RUNTIME="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly LINKER="${PROJECT_ROOT}/tests/sculptor-gpt2-8x8/tile-64m.ld"

require_file "${BASELINE_CORES}/core-0.o"
require_file "${CANDIDATE_LL}"
"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
mkdir -p -- "${OUT}"

common=(
    "--target=${GOLEM_TARGET}" "-mcpu=${GOLEM_CPU}" "-mabi=${GOLEM_ABI}"
    -mcmodel=medany -ffreestanding -fno-stack-protector
    -ffunction-sections -fdata-sections -O3
)
cxx=(
    "${common[@]}" -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
    -fno-asynchronous-unwind-tables -Wall -Wextra -Wpedantic -Werror
    "-I${INSTALL_ROOT}/runtime/include" "-I${PROJECT_ROOT}/platform"
)

"${LLVM}/bin/clang" "${common[@]}" -c "${PROJECT_ROOT}/platform/crt0.S" -o "${OUT}/crt0.o"
for source in uart platform-exit freestanding-memory mlir-runtime; do
    "${LLVM}/bin/clang++" "${cxx[@]}" -c "${PROJECT_ROOT}/platform/${source}.cpp" -o "${OUT}/${source}.o"
done
"${LLVM}/bin/clang++" "${cxx[@]}" -fno-builtin-expf -fno-builtin-erff \
    -c "${PROJECT_ROOT}/tests/sculptor-gpt2-8x8/freestanding-math.cpp" -o "${OUT}/freestanding-math.o"
"${LLVM}/bin/clang++" "${cxx[@]}" -c "${TEST_DIR}/main.cpp" -o "${OUT}/main.o"
"${LLVM}/bin/clang" "${common[@]}" -Wno-override-module -c "${CANDIDATE_LL}" -o "${OUT}/candidate-core.o"

link_one() {
    local name="$1"
    local core_object="$2"
    "${LLVM}/bin/clang++" "${common[@]}" -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${LINKER}" "${OUT}/crt0.o" "${OUT}/uart.o" \
        "${OUT}/platform-exit.o" "${OUT}/freestanding-memory.o" \
        "${OUT}/freestanding-math.o" "${OUT}/mlir-runtime.o" \
        "${OUT}/main.o" "${core_object}" "${RUNTIME}" -o "${OUT}/${name}.elf"
}

link_one baseline "${BASELINE_CORES}/core-0.o"
link_one candidate "${OUT}/candidate-core.o"
echo "built isolated C3 baseline and candidate ELFs"
