#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "test-env.sh must be sourced, not run" >&2
    exit 2
fi

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# shellcheck source=../../build-scripts/common.sh
source "${TESTS_ROOT}/../build-scripts/common.sh"

readonly QEMU_RISCV_CPU="${GOLEM_QEMU_RISCV_CPU:-rv64,v=true,vext_spec=v1.0,vlen=256,elen=64}"
