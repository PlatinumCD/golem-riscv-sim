#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/.." && pwd)"
readonly SAMPLE="${PROJECT_ROOT}/web/data/sample.json"

"${TEST_DIR}/test-exporter.py"
if command -v node >/dev/null 2>&1; then
    node --check "${PROJECT_ROOT}/web/app.js"
    node --check "${PROJECT_ROOT}/web/lib.js"
fi

python3 "${PROJECT_ROOT}/exporter/export-profile.py" \
    "${TEST_DIR}/fixtures/profile" \
    "${SAMPLE}" \
    --title "Mittens flow demonstration" \
    --source "visualizer test fixture"

test -s "${SAMPLE}"
echo "Mittens visualization webpage assets: PASS"
