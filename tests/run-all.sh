#!/usr/bin/env bash
set -euo pipefail

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -B "${TESTS_ROOT}/../tools/hardware/hardware_runner.py" "$@"
