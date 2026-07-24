#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PREPARED_SOURCE_ROOT}/torch-mlir"
readonly BUILD="${BUILD_ROOT}/torch-mlir"
readonly INSTALL="${INSTALL_ROOT}/torch-mlir"
readonly LLVM_INSTALL="${INSTALL_ROOT}/llvm"
readonly LLVM_LIT="${BUILD_ROOT}/llvm/bin/llvm-lit"
readonly HOST_PYTHON="${TORCH_MLIR_HOST_PYTHON:-/usr/bin/python3}"

for command in cmake git ninja; do
    require_command "${command}"
done
require_executable "${HOST_PYTHON}"
"${SCRIPT_DIR}/prepare-torch-mlir.sh"
require_file "${LLVM_INSTALL}/lib/cmake/llvm/LLVMConfig.cmake"
require_file "${LLVM_INSTALL}/lib/cmake/mlir/MLIRConfig.cmake"
require_executable "${LLVM_INSTALL}/bin/mlir-tblgen"
require_executable "${LLVM_LIT}"

cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DLLVM_DIR="${LLVM_INSTALL}/lib/cmake/llvm" \
    -DMLIR_DIR="${LLVM_INSTALL}/lib/cmake/mlir" \
    -DLLVM_EXTERNAL_LIT="${LLVM_LIT}" \
    -DMLIR_TABLEGEN_EXE="${LLVM_INSTALL}/bin/mlir-tblgen" \
    -DPython3_EXECUTABLE="${HOST_PYTHON}" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DMLIR_ENABLE_BINDINGS_PYTHON=OFF \
    -DTORCH_MLIR_OUT_OF_TREE_BUILD=ON \
    -DTORCH_MLIR_ENABLE_STABLEHLO=OFF \
    -DTORCH_MLIR_ENABLE_TOSA=ON \
    -DTORCH_MLIR_ENABLE_REFBACKEND=ON \
    -DTORCH_MLIR_ENABLE_PYTORCH_EXTENSIONS=OFF \
    -DTORCH_MLIR_ENABLE_JIT_IR_IMPORTER=OFF \
    -DTORCH_MLIR_ENABLE_LTC=OFF

cmake --build "${BUILD}" --parallel "${BUILD_JOBS}"
cmake --install "${BUILD}"

require_executable "${INSTALL}/bin/torch-mlir-opt"
echo "installed Torch-MLIR: ${INSTALL}"
