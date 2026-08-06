#!/usr/bin/env bash
set -euo pipefail

readonly MOVIE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${MOVIE_DIR}/../.." && pwd)"
readonly CONFIGURATION="greedy-timing-l3-b8-all-heuristics"
readonly OUTPUT_DIRECTORY="${PROJECT_ROOT}/build/movies/gpt2-8x8-${CONFIGURATION}"
readonly TRACE="${OUTPUT_DIRECTORY}/${CONFIGURATION}-trace.json"
readonly VENV="${PROJECT_ROOT}/build/visualizer-movie-venv"
readonly PYTHON="${VENV}/bin/python"

if [[ ! -f "${TRACE}" ]]; then
    echo "missing source trace: ${TRACE}" >&2
    echo "run ./visualizer/movie/build-gpt2-8x8-movie.sh first" >&2
    exit 1
fi
if [[ ! -x "${PYTHON}" ]]; then
    python3 -m venv "${VENV}"
fi
"${PYTHON}" -m pip install \
    --disable-pip-version-check \
    --quiet \
    -r "${MOVIE_DIR}/requirements.txt"

render_variant() {
    local name="$1"
    local seconds="$2"
    local movie="${OUTPUT_DIRECTORY}/${CONFIGURATION}-${name}.mp4"
    local manifest="${OUTPUT_DIRECTORY}/${CONFIGURATION}-${name}-manifest.json"

    "${PYTHON}" "${MOVIE_DIR}/render-movie.py" \
        "${TRACE}" \
        "${movie}" \
        --seconds "${seconds}" \
        --fps 30 \
        --width 1920 \
        --height 1080 \
        --crf 17 \
        --manifest "${manifest}"
}

# These multipliers are relative to the canonical 90-second movie.
render_variant half-speed 180
render_variant 4x-speed 22.5

echo "Half-speed movie: ${OUTPUT_DIRECTORY}/${CONFIGURATION}-half-speed.mp4"
echo "4x-speed movie: ${OUTPUT_DIRECTORY}/${CONFIGURATION}-4x-speed.mp4"
