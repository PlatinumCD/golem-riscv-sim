#!/usr/bin/env bash
# One entry point for the reproducible RISC-V OpenMP/QEMU demonstration.
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ACTION="${1:-all}"

need() { command -v "$1" >/dev/null || { echo "Missing required command: $1" >&2; exit 1; }; }
for command in git cmake ninja make gcc g++ qemu-system-riscv64 cpio gzip; do need "$command"; done

case "$ACTION" in
  all)
    git -C "$ROOT" submodule update --init third_party/llvm-project third_party/riscv-gnu-toolchain third_party/linux third_party/sst-core third_party/sst-elements
    "$ROOT/scripts/build-llvm.sh"; "$ROOT/scripts/build-musl-toolchain.sh"; "$ROOT/scripts/build-libomp.sh"; "$ROOT/scripts/build-kernel.sh"; "$ROOT/scripts/build-sst-core.sh"; "$ROOT/scripts/build-sst-elements.sh" ;;
  toolchain)
    git -C "$ROOT" submodule update --init third_party/llvm-project third_party/riscv-gnu-toolchain
    "$ROOT/scripts/build-llvm.sh"; "$ROOT/scripts/build-musl-toolchain.sh"; "$ROOT/scripts/build-libomp.sh" ;;
  sst-core)
    git -C "$ROOT" submodule update --init third_party/sst-core
    "$ROOT/scripts/build-sst-core.sh" ;;
  sst)
    git -C "$ROOT" submodule update --init third_party/sst-core third_party/sst-elements
    "$ROOT/scripts/build-sst-core.sh"; "$ROOT/scripts/build-sst-elements.sh" ;;
  initramfs) "$ROOT/scripts/build-initramfs.sh" ;;
  run) "$ROOT/scripts/build-initramfs.sh"; "$ROOT/scripts/run-qemu.sh" ;;
  *) echo "Usage: $0 [all|toolchain|sst-core|sst|initramfs|run]" >&2; exit 2 ;;
esac
