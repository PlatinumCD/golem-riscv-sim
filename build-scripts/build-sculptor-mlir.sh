#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PROJECT_ROOT}/third_party/sculptor-mlir"
readonly BUILD="${BUILD_ROOT}/sculptor-mlir"
readonly INSTALL="${INSTALL_ROOT}/sculptor-mlir"
readonly LLVM="${INSTALL_ROOT}/llvm"
readonly HOST_CC="${SCULPTOR_HOST_CC:-/usr/bin/cc}"
readonly HOST_CXX="${SCULPTOR_HOST_CXX:-/usr/bin/c++}"

for command in cmake git ninja; do
    require_command "${command}"
done
require_executable "${HOST_CC}"
require_executable "${HOST_CXX}"
require_git_commit "${SOURCE}" "${SCULPTOR_MLIR_COMMIT}" "Sculptor-MLIR"
require_file "${LLVM}/lib/cmake/llvm/LLVMConfig.cmake"
require_file "${LLVM}/lib/cmake/mlir/MLIRConfig.cmake"
require_executable "${LLVM}/bin/mlir-tblgen"

cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DCMAKE_C_COMPILER="${HOST_CC}" \
    -DCMAKE_CXX_COMPILER="${HOST_CXX}" \
    -DLLVM_DIR="${LLVM}/lib/cmake/llvm" \
    -DMLIR_DIR="${LLVM}/lib/cmake/mlir" \
    -DMLIR_TABLEGEN_EXE="${LLVM}/bin/mlir-tblgen" \
    -DSCULPTOR_MLIR_BUILD_RUNTIME=ON

cmake --build "${BUILD}" --parallel "${BUILD_JOBS}"
cmake --install "${BUILD}"

require_executable "${INSTALL}/bin/sculptor-mlir-opt"
require_file "${INSTALL}/lib/libgolem-runtime.a"
for pass in \
    --sculptor-build-ra-tree \
    --sculptor-plan-mapping \
    --sculptor-place-logical-tiles \
    --sculptor-outline-tile-routines \
    --sculptor-materialize-tile-runtime-graph; do
    if ! "${INSTALL}/bin/sculptor-mlir-opt" --help | grep -- "${pass}" >/dev/null; then
        echo "installed Sculptor-MLIR tool does not expose ${pass}" >&2
        exit 1
    fi
done

echo "installed Sculptor-MLIR: ${INSTALL}"
