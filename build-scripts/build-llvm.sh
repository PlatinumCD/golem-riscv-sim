#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${GOLEM_LLVM_SOURCE:-${PROJECT_ROOT}/third_party/llvm-project}"
readonly BUILD="${BUILD_ROOT}/llvm"
readonly INSTALL="${INSTALL_ROOT}/llvm"
readonly RISCV_GNU_TOOLCHAIN="${GOLEM_RISCV_GNU_TOOLCHAIN_DIR:-${INSTALL_ROOT}/riscv-gnu-toolchain}"
require_owned_comparison_output "${INSTALL}" "${INSTALL_ROOT}"
require_owned_comparison_output "${BUILD}" "${BUILD_ROOT}"

for command in cmake ninja git; do
require_command "${command}"
done
"${SCRIPT_DIR}/build-compiler-python.sh"
require_git_commit "${SOURCE}" "${LLVM_COMMIT}" "LLVM"
if [[ "${SOURCE}" == "${PROJECT_ROOT}/third_party/llvm-project" ]]; then
    require_clean_submodule "${SOURCE}" "LLVM"
fi
require_executable "${RISCV_GNU_TOOLCHAIN}/bin/${GOLEM_TARGET}-gcc"
require_executable "${RISCV_GNU_TOOLCHAIN}/bin/${GOLEM_TARGET}-g++"
require_file "${RISCV_GNU_TOOLCHAIN}/${GOLEM_TARGET}/include/stdint.h"

readonly PYTHON_INCLUDE="$("${COMPILER_PYTHON}" -c \
    'import sysconfig; print(sysconfig.get_path("include"))')"
readonly PYTHON_LIBRARY="$("${COMPILER_PYTHON}" -c \
    'import os, sysconfig; print(os.path.join(sysconfig.get_config_var("LIBDIR"), sysconfig.get_config_var("LDLIBRARY")))')"

cmake -S "${SOURCE}/llvm" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DLLVM_ENABLE_PROJECTS='clang;lld;mlir' \
    -DLLVM_TARGETS_TO_BUILD=RISCV \
    -DLLVM_DEFAULT_TARGET_TRIPLE="${GOLEM_TARGET}" \
    -DGCC_INSTALL_PREFIX="${RISCV_GNU_TOOLCHAIN}" \
    -DUSE_DEPRECATED_GCC_INSTALL_PREFIX=ON \
    -DDEFAULT_SYSROOT="${RISCV_GNU_TOOLCHAIN}/${GOLEM_TARGET}" \
    -DLLVM_BUILD_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DMLIR_ENABLE_BINDINGS_PYTHON=ON \
    -DPython_EXECUTABLE="${COMPILER_PYTHON}" \
    -DPython_INCLUDE_DIR="${PYTHON_INCLUDE}" \
    -DPython_LIBRARY="${PYTHON_LIBRARY}" \
    -DPython3_EXECUTABLE="${COMPILER_PYTHON}"

cmake --build "${BUILD}" --parallel "${BUILD_JOBS}"
cmake --install "${BUILD}"

require_executable "${INSTALL}/bin/clang++"
require_executable "${INSTALL}/bin/ld.lld"
require_executable "${INSTALL}/bin/llvm-readelf"
require_file "${INSTALL}/python_packages/mlir_core/mlir/ir.py"
PYTHONPATH="${INSTALL}/python_packages/mlir_core" \
    "${COMPILER_PYTHON}" -c 'from mlir import ir'
echo "installed Golem LLVM: ${INSTALL}"
