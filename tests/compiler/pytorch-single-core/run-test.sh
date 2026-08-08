#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/pytorch-single-core"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly TORCH_MLIR="${INSTALL_ROOT}/torch-mlir"
readonly TORCH_MLIR_PYTHON="${TORCH_MLIR}/python_packages/torch_mlir"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly LINALG_MLIR="${OUTPUT_DIR}/model-linalg.mlir"
readonly LLVM_MLIR="${OUTPUT_DIR}/model-llvm.mlir"
readonly LLVM_IR="${OUTPUT_DIR}/model.ll"
readonly ELF="${OUTPUT_DIR}/pytorch-single-core.elf"
readonly LOG="${OUTPUT_DIR}/qemu.log"

for executable in \
    "${COMPILER_PYTHON}" \
    "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" \
    "${LLVM}/bin/mlir-opt" \
    "${LLVM}/bin/mlir-translate" \
    "${LLVM}/bin/llvm-readelf" \
    "${QEMU}"; do
    require_executable "${executable}"
done
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"

mkdir -p -- "${OUTPUT_DIR}"

PYTHONPATH="${TORCH_MLIR_PYTHON}" \
    "${COMPILER_PYTHON}" \
    "${TEST_DIR}/import-model.py" \
    "${LINALG_MLIR}"

if ! grep -q 'linalg.generic' "${LINALG_MLIR}"; then
    echo "Torch-MLIR output does not contain the expected Linalg computation" >&2
    exit 1
fi

"${LLVM}/bin/mlir-opt" "${LINALG_MLIR}" \
    --one-shot-bufferize='bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map' \
    --buffer-results-to-out-params \
    --promote-buffers-to-stack='max-alloc-size-in-bytes=4096' \
    --convert-linalg-to-loops \
    --lower-affine \
    --convert-scf-to-cf \
    --llvm-request-c-wrappers \
    --convert-to-llvm \
    --reconcile-unrealized-casts \
    -o "${LLVM_MLIR}"

"${LLVM}/bin/mlir-translate" \
    --mlir-to-llvmir \
    "${LLVM_MLIR}" \
    -o "${LLVM_IR}"

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

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
"${LLVM}/bin/clang++" "${common_flags[@]}" \
    -std=c++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    "-I${PROJECT_ROOT}/platform" \
    -c "${PROJECT_ROOT}/platform/uart.cpp" \
    -o "${OUTPUT_DIR}/uart.o"
"${LLVM}/bin/clang++" "${common_flags[@]}" \
    -std=c++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    "-I${PROJECT_ROOT}/platform" \
    -c "${PROJECT_ROOT}/platform/platform-exit.cpp" \
    -o "${OUTPUT_DIR}/platform-exit.o"
"${LLVM}/bin/clang++" "${common_flags[@]}" \
    -std=c++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    -Wall -Wextra -Wpedantic \
    "-I${PROJECT_ROOT}/platform" \
    -c "${TEST_DIR}/main.cpp" \
    -o "${OUTPUT_DIR}/main.o"
"${LLVM}/bin/clang" "${common_flags[@]}" \
    -Wno-override-module \
    -c "${LLVM_IR}" \
    -o "${OUTPUT_DIR}/model.o"

"${LLVM}/bin/clang++" "${common_flags[@]}" \
    -nostdlib -nostartfiles -nodefaultlibs \
    -fuse-ld=lld \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
    "-Wl,-Map,${OUTPUT_DIR}/pytorch-single-core.map" \
    "${OUTPUT_DIR}/crt0.o" \
    "${OUTPUT_DIR}/uart.o" \
    "${OUTPUT_DIR}/platform-exit.o" \
    "${OUTPUT_DIR}/main.o" \
    "${OUTPUT_DIR}/model.o" \
    -o "${ELF}"

entry="$("${LLVM}/bin/llvm-readelf" --file-header "${ELF}" |
    awk '/Entry point address:/ { print $4 }')"
if [[ "${entry}" != "0x80000000" ]]; then
    echo "unexpected RISC-V ELF entry point: ${entry}" >&2
    exit 1
fi
if "${LLVM}/bin/llvm-readelf" --program-headers "${ELF}" | grep -q INTERP; then
    echo "PyTorch proof unexpectedly contains a dynamic interpreter" >&2
    exit 1
fi

"${QEMU}" \
    -machine virt \
    -cpu "${QEMU_RISCV_CPU}" \
    -smp 1 \
    -m 16M \
    -bios none \
    -kernel "${ELF}" \
    -display none \
    -monitor none \
    -serial stdio \
    -no-reboot |
    tee "${LOG}"

grep -Fq 'output [12, 4]' "${LOG}"
grep -Fq 'PYTORCH_SINGLE_CORE_PASS' "${LOG}"

echo "PyTorch -> Torch-MLIR -> LLVM -> single-hart RISC-V executable: PASS"
