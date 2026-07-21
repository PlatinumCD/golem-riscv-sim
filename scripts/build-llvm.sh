#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE="$ROOT/third_party/llvm-project"
readonly BUILD="$ROOT/build/llvm"
readonly INSTALL="$ROOT/build/toolchain/llvm-golem-analog"
readonly JOBS="${JOBS:-$(nproc)}"

cmake -S "$SOURCE/llvm" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DLLVM_ENABLE_PROJECTS='clang;lld;mlir' \
  -DLLVM_TARGETS_TO_BUILD=RISCV \
  -DLLVM_DEFAULT_TARGET_TRIPLE=riscv64-unknown-linux-gnu \
  -DLLVM_BUILD_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF
cmake --build "$BUILD" --parallel "$JOBS"
cmake --install "$BUILD"
