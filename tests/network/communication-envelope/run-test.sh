#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 -B -m unittest discover -s "${TEST_DIR}/tests" -v
if [[ "${1:-}" == --host-only ]]; then
    exit 0
fi
exec python3 -B "${TEST_DIR}/run.py" --regression "$@"
