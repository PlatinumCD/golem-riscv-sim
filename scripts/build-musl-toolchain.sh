#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE="$ROOT/third_party/riscv-gnu-toolchain"
readonly BUILD="$ROOT/build/riscv-gnu-toolchain"
readonly INSTALL="$ROOT/build/toolchain/riscv64-musl-toolchain"
readonly PATCH="$ROOT/patches/riscv-gnu-toolchain-stage2-posix.patch"
readonly JOBS="${JOBS:-$(nproc)}"

if git -C "$SOURCE" apply --check "$PATCH"; then
  git -C "$SOURCE" apply "$PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$PATCH"; then
  echo 'The stage-2 pthread patch does not apply to this pinned toolchain.' >&2
  exit 1
fi

mkdir -p "$BUILD"
cd "$BUILD"
"$SOURCE/configure" --prefix="$INSTALL" --with-arch=rv64gc --with-abi=lp64d \
  --with-languages=c,c++ --disable-gdb --disable-multilib --enable-strip
GCC_STAGE2_EXTRA_CONFIGURE_FLAGS=--enable-threads=posix make -j "$JOBS" musl
test -e "$INSTALL/sysroot/lib/libc.so"
