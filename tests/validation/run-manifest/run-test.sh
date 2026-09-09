#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"

"${PYTHON:-python3}" "${TEST_DIR}/test-manifest.py"

echo "sculptor run manifest: PASS"
