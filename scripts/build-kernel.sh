#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE="$ROOT/third_party/linux"
readonly BUILD="$ROOT/build/linux"
readonly LLVM="$ROOT/build/toolchain/llvm-golem-analog"
readonly JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$BUILD/.config" ]]; then
  make -C "$SOURCE" O="$BUILD" ARCH=riscv LLVM="$LLVM/bin/" \
    HOSTCC=gcc HOSTCXX=g++ defconfig
fi
"$SOURCE/scripts/config" --file "$BUILD/.config" --set-val NR_CPUS 128
make -C "$SOURCE" O="$BUILD" ARCH=riscv LLVM="$LLVM/bin/" \
  HOSTCC=gcc HOSTCXX=g++ olddefconfig
make -C "$SOURCE" O="$BUILD" ARCH=riscv LLVM="$LLVM/bin/" \
  HOSTCC=gcc HOSTCXX=g++ -j "$JOBS" Image
test -f "$BUILD/arch/riscv/boot/Image"
