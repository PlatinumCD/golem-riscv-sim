#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

required_commands=(
    autoconf
    automake
    autoreconf
    cmake
    find
    g++
    gcc
    git
    install
    libtoolize
    make
    mpicc
    mpicxx
    ninja
    pkg-config
    python3
)

missing=0
for command in "${required_commands[@]}"; do
    if ! require_command "${command}"; then
        missing=1
    fi
done

if [[ "$(uname -m)" != "aarch64" && "${ALLOW_UNSUPPORTED_HOST:-0}" != "1" ]]; then
    echo "Platform v0 builds are validated on an aarch64 host; found $(uname -m)" >&2
    echo "set ALLOW_UNSUPPORTED_HOST=1 to override" >&2
    missing=1
fi

if ((missing != 0)); then
    exit 1
fi

echo "host dependency check passed (${BUILD_JOBS} build jobs)"
