#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
# shellcheck source=parameters.sh
source "${TEST_DIR}/parameters.sh"
MEMORY_BACKEND="${GOLEM_MODEL_MEMORY_BACKEND}"
case "${1:-}" in
    "") ;;
    --memhierarchy)
        MEMORY_BACKEND="memhierarchy"
        shift
        ;;
    --memory-backend=*)
        MEMORY_BACKEND="${1#*=}"
        shift
        ;;
    *)
        echo "usage: run-all.sh [--memhierarchy]" >&2
        exit 2
        ;;
esac
if [[ "$#" -ne 0 ]] ||
   [[ "${MEMORY_BACKEND}" != "native" &&
      "${MEMORY_BACKEND}" != "memhierarchy" ]]; then
    echo "memory backend must be native or memhierarchy" >&2
    exit 2
fi
readonly RESULTS_ROOT="${BUILD_ROOT}/tests/sculptor-ra-tree-model-suite/${MEMORY_BACKEND}"
readonly RUN_TAG="mesh-${GOLEM_MODEL_MESH_ROWS}x${GOLEM_MODEL_MESH_COLS}-dw${GOLEM_MODEL_DIGITAL_WORKERS}-balance${GOLEM_MODEL_BALANCE_DIGITAL_WORK}-router${GOLEM_MODEL_MESH_ROUTER_BACKEND}-sst${GOLEM_MODEL_SST_THREADS}"

readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
mapfile -t CASES < <(
    PYTHONPATH="${TORCH_MLIR_PYTHON}" "${COMPILER_PYTHON}" \
        "${TEST_DIR}/import-model.py" --list
)

if [[ "${#CASES[@]}" -eq 0 ]]; then
    echo "could not enumerate the non-GPT Sculptor model cases" >&2
    exit 1
fi
mkdir -p -- "${RESULTS_ROOT}"
for case_name in "${CASES[@]}"; do
    "${TEST_DIR}/run-test.sh" "--memory-backend=${MEMORY_BACKEND}" "${case_name}"
done
summary_tmp="${RESULTS_ROOT}/simulation-runtimes.csv.tmp"
head -n1 "${RESULTS_ROOT}/${CASES[0]}/${RUN_TAG}/result.csv" >"${summary_tmp}"
for case_name in "${CASES[@]}"; do
    tail -n1 "${RESULTS_ROOT}/${case_name}/${RUN_TAG}/result.csv" >>"${summary_tmp}"
done
mv -- "${summary_tmp}" "${RESULTS_ROOT}/simulation-runtimes.csv"
echo "recorded ${#CASES[@]} ${MEMORY_BACKEND} non-GPT model simulations: ${RESULTS_ROOT}/simulation-runtimes.csv"
