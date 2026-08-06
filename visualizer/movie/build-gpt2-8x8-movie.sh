#!/usr/bin/env bash
set -euo pipefail

readonly MOVIE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${MOVIE_DIR}/../.." && pwd)"
readonly CONFIGURATION="greedy-timing-l3-b8-all-heuristics"
readonly DEFAULT_LOWERING="${PROJECT_ROOT}/build/tests/sculptor-gpt2-scheduling-sweep/tokens-4/configurations/${CONFIGURATION}/analog"
readonly DEFAULT_DEPLOYMENT="${DEFAULT_LOWERING}/deployment-movie-trace"
readonly DEFAULT_PROFILE="${DEFAULT_DEPLOYMENT}/performance-profile"
readonly DEFAULT_TASK_IR="${DEFAULT_LOWERING}/cores"
readonly DEFAULT_SCHEDULED_IR="${DEFAULT_LOWERING}/10-scheduled.mlir"
readonly DEFAULT_CONFIGURATION_METADATA="$(
    dirname -- "${DEFAULT_LOWERING}"
)/configuration.txt"
readonly DEFAULT_OUTPUT="${PROJECT_ROOT}/build/movies/gpt2-8x8-${CONFIGURATION}"

PROFILE="${MITTENS_MOVIE_PROFILE:-${DEFAULT_PROFILE}}"
TASK_IR="${MITTENS_MOVIE_TASK_IR:-${DEFAULT_TASK_IR}}"
SCHEDULED_IR="${MITTENS_MOVIE_SCHEDULED_IR:-${DEFAULT_SCHEDULED_IR}}"
OUTPUT_DIRECTORY="${MITTENS_MOVIE_OUTPUT_DIRECTORY:-${DEFAULT_OUTPUT}}"
MOVIE_SECONDS="${MITTENS_MOVIE_SECONDS:-90}"
MOVIE_FPS="${MITTENS_MOVIE_FPS:-30}"
MOVIE_WIDTH="${MITTENS_MOVIE_WIDTH:-1920}"
MOVIE_HEIGHT="${MITTENS_MOVIE_HEIGHT:-1080}"
MOVIE_CRF="${MITTENS_MOVIE_CRF:-17}"
VENV="${PROJECT_ROOT}/build/visualizer-movie-venv"
TRACE="${OUTPUT_DIRECTORY}/${CONFIGURATION}-trace.json"
MOVIE="${OUTPUT_DIRECTORY}/${CONFIGURATION}.mp4"
PREVIEW="${OUTPUT_DIRECTORY}/${CONFIGURATION}-preview.png"
MANIFEST="${OUTPUT_DIRECTORY}/${CONFIGURATION}-manifest.json"

if [[ ! -f "${PROFILE}/summary.json" ]]; then
    echo "missing GPT-2 performance profile: ${PROFILE}" >&2
    echo "run the GPT-2 deployment with MITTENS_VISUALIZATION_EXPORT=1 first" >&2
    exit 1
fi

if [[ ! -d "${TASK_IR}" && ! -f "${TASK_IR}" ]]; then
    echo "missing GPT-2 task IR: ${TASK_IR}" >&2
    exit 1
fi
if [[ ! -f "${SCHEDULED_IR}" ]]; then
    echo "missing GPT-2 scheduled IR: ${SCHEDULED_IR}" >&2
    echo "run ./visualizer/movie/prepare-gpt2-8x8-source.sh first" >&2
    exit 1
fi

# Do not permit another anonymous fixture to be mislabeled as this film.
for expected in \
    'sculptor.schedule.greedy_lookahead = 3 : i64' \
    'sculptor.schedule.greedy_beam_width = 8 : i64' \
    'sculptor.schedule.mesh_rows = 8 : i64' \
    'sculptor.schedule.mesh_cols = 8 : i64' \
    'sculptor.schedule.placement_cost_mode = "analog"'; do
    if ! grep -Fqm1 -- "${expected}" "${SCHEDULED_IR}"; then
        echo "movie source fails schedule provenance check: ${expected}" >&2
        exit 1
    fi
done
for heuristic in \
    transfer-cost \
    boundary-regret \
    compact-region \
    link-pressure \
    scope=diagonal; do
    if ! grep -Fm1 'sculptor.schedule.greedy_heuristic' "${SCHEDULED_IR}" |
       grep -Fq -- "${heuristic}"; then
        echo "movie source is missing scheduler heuristic: ${heuristic}" >&2
        exit 1
    fi
done
if [[ "${SCHEDULED_IR}" == "${DEFAULT_SCHEDULED_IR}" ]]; then
    if [[ ! -f "${DEFAULT_CONFIGURATION_METADATA}" ]] ||
       ! grep -Fxq 'balanced_reductions=1' \
           "${DEFAULT_CONFIGURATION_METADATA}"; then
        echo "movie source is missing balanced-reduction provenance" >&2
        exit 1
    fi
fi

mkdir -p "${OUTPUT_DIRECTORY}"

if [[ ! -x "${VENV}/bin/python" ]]; then
    python3 -m venv "${VENV}"
fi
"${VENV}/bin/python" -m pip install \
    --disable-pip-version-check \
    --quiet \
    -r "${MOVIE_DIR}/requirements.txt"

python3 "${PROJECT_ROOT}/visualizer/exporter/export-profile.py" \
    "${PROFILE}" \
    "${TRACE}" \
    --width 8 \
    --height 8 \
    --title "GPT-2 · greedy-timing L3/B8 · all heuristics" \
    --source "$(realpath --relative-to="${PROJECT_ROOT}" "${PROFILE}")" \
    --task-ir "${TASK_IR}"

"${VENV}/bin/python" "${MOVIE_DIR}/render-movie.py" \
    "${TRACE}" \
    "${MOVIE}" \
    --seconds "${MOVIE_SECONDS}" \
    --fps "${MOVIE_FPS}" \
    --width "${MOVIE_WIDTH}" \
    --height "${MOVIE_HEIGHT}" \
    --crf "${MOVIE_CRF}" \
    --preview "${PREVIEW}" \
    --manifest "${MANIFEST}"

echo "GPT-2 movie: ${MOVIE}"
echo "Preview frame: ${PREVIEW}"
echo "Movie manifest: ${MANIFEST}"
