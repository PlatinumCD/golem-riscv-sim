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
readonly HOST_PYTHON="${TORCH_MLIR_HOST_PYTHON:-${COMPILER_PYTHON}}"

for command in cmake git ninja; do
    require_command "${command}"
done
"${SCRIPT_DIR}/build-compiler-python.sh"
require_executable "${HOST_PYTHON}"
"${SCRIPT_DIR}/prepare-torch-mlir.sh"
require_file "${LLVM_INSTALL}/lib/cmake/llvm/LLVMConfig.cmake"
require_file "${LLVM_INSTALL}/lib/cmake/mlir/MLIRConfig.cmake"
require_executable "${LLVM_INSTALL}/bin/mlir-tblgen"
require_executable "${LLVM_LIT}"

readonly PYTHON_INCLUDE="$("${HOST_PYTHON}" -c \
    'import sysconfig; print(sysconfig.get_path("include"))')"
readonly PYTHON_LIBRARY="$("${HOST_PYTHON}" -c \
    'import os, sysconfig; print(os.path.join(sysconfig.get_config_var("LIBDIR"), sysconfig.get_config_var("LDLIBRARY")))')"

cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DLLVM_DIR="${LLVM_INSTALL}/lib/cmake/llvm" \
    -DMLIR_DIR="${LLVM_INSTALL}/lib/cmake/mlir" \
    -DLLVM_EXTERNAL_LIT="${LLVM_LIT}" \
    -DMLIR_TABLEGEN_EXE="${LLVM_INSTALL}/bin/mlir-tblgen" \
    -DPython_EXECUTABLE="${HOST_PYTHON}" \
    -DPython_INCLUDE_DIR="${PYTHON_INCLUDE}" \
    -DPython_LIBRARY="${PYTHON_LIBRARY}" \
    -DPython3_EXECUTABLE="${HOST_PYTHON}" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DMLIR_ENABLE_BINDINGS_PYTHON=ON \
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
require_file \
    "${INSTALL}/python_packages/torch_mlir/torch_mlir/compiler_utils.py"
PYTHONPATH="${INSTALL}/python_packages/torch_mlir" \
    "${HOST_PYTHON}" -c 'from torch_mlir import fx'
echo "installed Torch-MLIR: ${INSTALL}"
