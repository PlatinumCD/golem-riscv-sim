#!/usr/bin/env bash
set -euo pipefail
readonly PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
echo 'QEMU preparation is owned by src/components/riscv-qemu/build_qemu.py; building the current bridge.' >&2
exec bash "${PROJECT_DIR}/build-scripts/build-qemu.sh"
