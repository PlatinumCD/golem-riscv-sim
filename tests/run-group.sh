#!/usr/bin/env bash
set -euo pipefail

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly GROUP="${1:?usage: tests/run-group.sh GROUP [runner options]}"
shift
exec python3 -B "${TESTS_ROOT}/../tools/hardware/hardware_runner.py" --group "${GROUP}" "$@"
