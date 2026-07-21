#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly QEMU_CPUS="${QEMU_CPUS:-100}"
readonly QEMU_MEMORY="${QEMU_MEMORY:-1G}"
if (( QEMU_CPUS < 100 )); then
  echo 'The 10x10 OpenMP demo requires at least 100 QEMU CPUs.' >&2
  exit 2
fi
exec qemu-system-riscv64 -machine virt -m "$QEMU_MEMORY" -smp "$QEMU_CPUS" -nographic -no-reboot \
  -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
  -kernel "$ROOT/build/linux/arch/riscv/boot/Image" -initrd "$ROOT/emulation/initramfs.cpio.gz" \
  -append 'console=ttyS0 rdinit=/init'
