#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../build-scripts/common.sh
source "${TEST_DIR}/../../build-scripts/common.sh"

readonly OUT="${BUILD_ROOT}/tests/sculptor-gpt2-vector-isolated"
readonly BASELINE_LL="${BUILD_ROOT}/tests/sculptor-gpt2-factorial-ab-hoist-static-allocs/tokens-4/configurations/br0-cr0-lp0-rb0-dm0/cores/core-0.ll"
readonly CANDIDATE_FINALIZED="/tmp/gpt2-vectorized-core0-finalized.mlir"
readonly CANDIDATE_LL="${OUT}/candidate.ll"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly RUNTIME="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly LINKER="${PROJECT_ROOT}/tests/sculptor-gpt2-8x8/tile-64m.ld"

require_file "${BASELINE_LL}"
require_file "${CANDIDATE_FINALIZED}"
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

"${SCULPTOR_OPT}" "${CANDIDATE_FINALIZED}" \
    --canonicalize \
    --cse \
    --empty-tensor-to-alloc-tensor \
    "--one-shot-bufferize=bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map" \
    '--buffer-results-to-out-params=hoist-static-allocs' \
    --sculptor-vectorize-marked-elementwise \
    --convert-bufferization-to-memref \
    --buffer-hoisting \
    --buffer-loop-hoisting \
    --buffer-deallocation-pipeline \
    --optimize-allocation-liveness \
    --convert-linalg-to-loops \
    --lower-affine \
    --convert-scf-to-cf \
    --convert-vector-to-llvm \
    --convert-math-to-libm \
    --convert-math-to-llvm \
    --expand-strided-metadata \
    --lower-affine \
    --convert-arith-to-llvm \
    --convert-index-to-llvm \
    --convert-cf-to-llvm \
    --finalize-memref-to-llvm \
    --convert-func-to-llvm \
    --reconcile-unrealized-casts \
    -o "${OUT}/candidate-compute-llvm.mlir"
"${SCULPTOR_OPT}" "${OUT}/candidate-compute-llvm.mlir" \
    --sculptor-emit-golem-tile-abi \
    --sculptor-finalize-golem-intrinsics \
    -o "${OUT}/candidate-task-only.mlir"
"${LLVM}/bin/mlir-translate" --mlir-to-llvmir \
    "${OUT}/candidate-task-only.mlir" -o "${CANDIDATE_LL}"

for version in baseline candidate; do
    ll_variable="${version^^}_LL"
    ll_path="${!ll_variable}"
    "${LLVM}/bin/clang" "${common[@]}" -fno-vectorize -fno-slp-vectorize \
        -Wno-override-module -c "${ll_path}" -o "${OUT}/${version}-core.o"
done

link_one() {
    local name="$1"
    "${LLVM}/bin/clang++" "${common[@]}" -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${LINKER}" "${OUT}/crt0.o" "${OUT}/uart.o" \
        "${OUT}/platform-exit.o" "${OUT}/freestanding-memory.o" \
        "${OUT}/freestanding-math.o" "${OUT}/mlir-runtime.o" \
        "${OUT}/main.o" "${OUT}/${name}-core.o" "${RUNTIME}" \
        -o "${OUT}/${name}.elf"
}

link_one baseline
link_one candidate

baseline_disassembly="$(
    "${LLVM}/bin/llvm-objdump" -d --no-show-raw-insn \
        --disassemble-symbols=__golem_tile_execute_task_152 \
        "${OUT}/baseline.elf"
)"
candidate_disassembly="$(
    "${LLVM}/bin/llvm-objdump" -d --no-show-raw-insn \
        --disassemble-symbols=__golem_tile_execute_task_152 \
        "${OUT}/candidate.elf"
)"
if grep -Fq 'vle32.v' <<<"${baseline_disassembly}"; then
    echo "scalar baseline unexpectedly contains unit-stride vector memory operations" >&2
    exit 1
fi
for mnemonic in vle32.v vfadd.vv vse32.v; do
    if ! grep -Fq "${mnemonic}" <<<"${candidate_disassembly}"; then
        echo "vector candidate does not contain ${mnemonic}" >&2
        exit 1
    fi
done

echo "built isolated task-152 scalar and vector ELFs"
