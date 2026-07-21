#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec qemu-system-riscv64 -machine virt -m 512M -smp 5 -nographic -no-reboot \
  -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
  -kernel "$ROOT/emulation/kernel/Image" -initrd "$ROOT/emulation/initramfs.cpio.gz" \
  -append 'console=ttyS0 rdinit=/init'
