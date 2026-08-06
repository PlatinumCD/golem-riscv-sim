#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sweep-common.sh
source "${SCRIPT_DIR}/sweep-common.sh"

readonly DASHBOARD_SOURCE="${SCRIPT_DIR}/dashboard"
readonly DASHBOARD_OUTPUT="$(
    realpath -m -- \
        "${MITTENS_GPT2_SWEEP_DASHBOARD_OUTPUT:-${SWEEP_OUTPUT_ROOT}/dashboard}"
)"

require_file "${SWEEP_RESULTS}"
require_file "${SWEEP_CONFIGURATION_MANIFEST}"
require_file "${DASHBOARD_SOURCE}/index.html"
require_file "${DASHBOARD_SOURCE}/styles.css"
require_file "${DASHBOARD_SOURCE}/app.js"
require_file "${DASHBOARD_SOURCE}/generate-data.py"

mkdir -p -- "${DASHBOARD_OUTPUT}"
install -m 0644 \
    "${DASHBOARD_SOURCE}/index.html" \
    "${DASHBOARD_SOURCE}/styles.css" \
    "${DASHBOARD_SOURCE}/app.js" \
    "${DASHBOARD_OUTPUT}/"

"${COMPILER_PYTHON}" \
    "${DASHBOARD_SOURCE}/generate-data.py" \
    --results "${SWEEP_RESULTS}" \
    --configurations "${SWEEP_CONFIGURATION_MANIFEST}" \
    --output "${DASHBOARD_OUTPUT}/data.js"

echo "GPT-2 scheduling dashboard: ${DASHBOARD_OUTPUT}/index.html"
echo "Serve from the repository root with:"
echo "  python3 -m http.server 8000"
echo "Then open:"
echo "  http://127.0.0.1:8000/build/tests/sculptor-gpt2-scheduling-sweep/dashboard/"
