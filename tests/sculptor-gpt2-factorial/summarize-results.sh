#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=factorial-common.sh
source "${SCRIPT_DIR}/factorial-common.sh"

factorial_validate_settings
mkdir -p -- "${FACTORIAL_OUTPUT_ROOT}"
"${COMPILER_PYTHON}" \
    "${SCRIPT_DIR}/summarize-results.py" \
    "${FACTORIAL_OUTPUT_ROOT}" \
    "${FACTORIAL_RESULTS}"

