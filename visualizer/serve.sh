#!/usr/bin/env bash
set -euo pipefail

readonly VISUALIZER_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${VISUALIZER_DIR}/.." && pwd)"
readonly PORT="${1:-8000}"
readonly TRACE="${2:-${VISUALIZER_DIR}/web/data/sample.json}"

if [[ ! "${PORT}" =~ ^[1-9][0-9]*$ ]] || ((PORT > 65535)); then
    echo "port must be an integer from 1 to 65535" >&2
    exit 2
fi
if [[ ! -f "${TRACE}" ]]; then
    echo "missing visualization trace: ${TRACE}" >&2
    exit 1
fi

readonly RELATIVE_TRACE="$(realpath --relative-to="${PROJECT_ROOT}" "${TRACE}")"
echo "Mittens Flow Scope:"
echo "  http://127.0.0.1:${PORT}/visualizer/web/?data=/${RELATIVE_TRACE}"
echo "  http://0.0.0.0:${PORT}/visualizer/web/?data=/${RELATIVE_TRACE}"
cd -- "${PROJECT_ROOT}"
exec python3 -m http.server "${PORT}" --bind 0.0.0.0
