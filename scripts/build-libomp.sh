#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly TOOLCHAIN="$ROOT/build/toolchain/riscv64-musl-toolchain"
readonly LLVM="$ROOT/build/toolchain/llvm-golem-analog"
readonly BUILD="$ROOT/build/libomp-riscv"
readonly JOBS="${JOBS:-$(nproc)}"

cmake -S "$ROOT/third_party/llvm-project/openmp" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" \
  -DCMAKE_C_COMPILER_TARGET=riscv64-unknown-linux-musl \
  -DCMAKE_CXX_COMPILER_TARGET=riscv64-unknown-linux-musl \
  -DCMAKE_C_FLAGS="--gcc-toolchain=$TOOLCHAIN" \
  -DCMAKE_CXX_FLAGS="--gcc-toolchain=$TOOLCHAIN" \
  -DCMAKE_SYSROOT="$TOOLCHAIN/sysroot" \
  -DCMAKE_INSTALL_PREFIX="$TOOLCHAIN/sysroot/usr" \
  -DLIBOMP_ARCH=riscv64 -DLIBOMP_ENABLE_SHARED=OFF -DLIBOMP_USE_HWLOC=OFF \
  -DLIBOMP_OMPT_SUPPORT=OFF -DOPENMP_ENABLE_OMPT_TOOLS=OFF \
  -DOPENMP_ENABLE_LIBOMPTARGET=OFF
cmake --build "$BUILD" --target omp --parallel "$JOBS"
cmake --install "$BUILD"
