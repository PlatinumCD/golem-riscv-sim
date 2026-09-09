#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -B "${TEST_DIR}/run.py" "$@"
