#!/usr/bin/env bash
set -euo pipefail

readonly MOVIE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${MOVIE_DIR}/../.." && pwd)"
readonly VENV="${PROJECT_ROOT}/build/visualizer-movie-venv"

if [[ ! -x "${VENV}/bin/python" ]]; then
    python3 -m venv "${VENV}"
fi
"${VENV}/bin/python" -m pip install \
    --disable-pip-version-check \
    --quiet \
    -r "${MOVIE_DIR}/requirements.txt"

"${VENV}/bin/python" "${MOVIE_DIR}/tests/test-renderer.py"
