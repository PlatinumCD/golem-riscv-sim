#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SOURCE="${PROJECT_ROOT}/third_party/llvm-project"
readonly BUILD="${BUILD_ROOT}/llvm"
readonly INSTALL="${INSTALL_ROOT}/llvm"

for command in cmake ninja git; do
    require_command "${command}"
done
require_git_commit "${SOURCE}" "${LLVM_COMMIT}" "LLVM"
require_clean_submodule "${SOURCE}" "LLVM"

cmake -S "${SOURCE}/llvm" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DLLVM_ENABLE_PROJECTS='clang;lld;mlir' \
    -DLLVM_TARGETS_TO_BUILD=RISCV \
    -DLLVM_DEFAULT_TARGET_TRIPLE="${GOLEM_TARGET}" \
    -DLLVM_BUILD_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF

cmake --build "${BUILD}" --parallel "${BUILD_JOBS}"
cmake --install "${BUILD}"

require_executable "${INSTALL}/bin/clang++"
require_executable "${INSTALL}/bin/ld.lld"
require_executable "${INSTALL}/bin/llvm-readelf"
echo "installed Golem LLVM: ${INSTALL}"
