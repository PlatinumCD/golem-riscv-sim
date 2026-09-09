#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

"${PYTHON:-python3}" "${TEST_DIR}/test-classifier.py"

echo "SST progress classifier: PASS"
