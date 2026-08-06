#!/usr/bin/env bash
set -euo pipefail

readonly MOVIE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${MOVIE_DIR}/../.." && pwd)"
readonly CONFIGURATION="greedy-timing-l3-b8-all-heuristics"
readonly CONFIGURATION_MANIFEST="${MOVIE_DIR}/greedy-timing-l3-all-heuristics.tsv"
readonly MODE_DIR="${PROJECT_ROOT}/build/tests/sculptor-gpt2-scheduling-sweep/tokens-4/configurations/${CONFIGURATION}/analog"
readonly DEPLOYMENT_DIR="${MODE_DIR}/deployment-movie-trace"

# Keep the historical 112-deployment experiment unchanged. This focused
# manifest adds the exact movie placement as a separate reproducible build.
MITTENS_GPT2_SWEEP_CONFIGURATION_MANIFEST="${CONFIGURATION_MANIFEST}" \
MITTENS_GPT2_SWEEP_TOKENS=4 \
MITTENS_GPT2_SWEEP_CONFIGS="${CONFIGURATION}" \
MITTENS_GPT2_SWEEP_MODES=analog \
MITTENS_GPT2_SWEEP_KEEP_IR=1 \
    "${PROJECT_ROOT}/tests/sculptor-gpt2-scheduling-sweep/build-test.sh"

# Generate the measured trace in the foreground. The trace therefore comes
# from precisely the objects and route manifest built above.
MITTENS_GPT2_LOWERING_DIR="${MODE_DIR}" \
MITTENS_GPT2_DEPLOYMENT_DIR="${DEPLOYMENT_DIR}" \
MITTENS_GPT2_SEQUENCE_LENGTH=4 \
MITTENS_GPT2_CPU_ISSUE_WIDTH=2 \
MITTENS_GPT2_CPU_CLOCK=1GHz \
MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES=100 \
MITTENS_GPT2_PROFILE_MODE=trace \
MITTENS_VISUALIZATION_EXPORT=1 \
MITTENS_MEMORY_BACKEND=native \
    "${PROJECT_ROOT}/tests/sculptor-gpt2-8x8/run-deployment.sh"

echo "Movie source lowering: ${MODE_DIR}"
echo "Movie source profile: ${DEPLOYMENT_DIR}/performance-profile"
