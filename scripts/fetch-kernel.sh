#!/usr/bin/env bash
set -euo pipefail
readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly OUTPUT="$ROOT/emulation/kernel/Image"
readonly URL='https://deb.debian.org/debian/dists/trixie/main/installer-riscv64/current/images/netboot/debian-installer/riscv64/linux'
mkdir -p "${OUTPUT%/*}"
curl --fail --location --output "$OUTPUT" "$URL"
