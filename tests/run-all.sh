#!/usr/bin/env bash
set -euo pipefail

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly GROUPS=(
    platform
    compiler
    runtime
    memory
    network
    analog
    models
    validation
)

for group in "${GROUPS[@]}"; do
    "${TESTS_ROOT}/run-group.sh" "${group}"
done

echo "all root test groups: PASS"
