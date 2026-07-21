#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly STAGING="$ROOT/build/initramfs-root"
readonly SOURCE="${TEST_SOURCE:-$ROOT/emulation/hundred-core-openmp.cpp}"
rm -rf "$STAGING"
mkdir -p "$STAGING/dev"
"$ROOT/toolchain/golem-clang" -static -fopenmp "$SOURCE" -o "$STAGING/test"
"$ROOT/toolchain/golem-clang" -static "$ROOT/emulation/init.cpp" -o "$STAGING/init"
(cd "$STAGING" && find . -print0 | cpio --null -o -H newc) | gzip -9 > "$ROOT/emulation/initramfs.cpio.gz"
