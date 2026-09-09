#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 "${TEST_DIR}/test-qualification.py"
echo "compile qualification validation: PASS"
