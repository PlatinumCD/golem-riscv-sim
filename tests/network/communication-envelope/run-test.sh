#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
python3 -B -m unittest discover -s "${PROJECT_ROOT}/studies/compute-communication/communication-envelope/tests" -v
exec python3 -B "${PROJECT_ROOT}/studies/compute-communication/communication-envelope/run.py" --regression "$@"
