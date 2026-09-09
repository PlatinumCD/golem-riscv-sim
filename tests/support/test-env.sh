#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "test-env.sh must be sourced, not run" >&2
    exit 2
fi

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# shellcheck source=../../build-scripts/common.sh
source "${TESTS_ROOT}/../build-scripts/common.sh"

readonly QEMU_RISCV_CPU="${GOLEM_QEMU_RISCV_CPU:-rv64,v=true,vext_spec=v1.0,vlen=256,elen=64}"

# Every study gets an isolated, descriptive results directory.  A suite may
# provide STUDY_RESULTS_DIR so all of its sub-runs share one run artifact.
study_results_dir() {
    local study_dir="$1"
    local study_name="$(basename -- "${study_dir}")"
    local suffix="${2:-}"
    if [[ -n "${STUDY_RESULTS_DIR:-}" ]]; then
        [[ -n "${suffix}" ]] && printf '%s\n' "${STUDY_RESULTS_DIR}/${suffix}" || printf '%s\n' "${STUDY_RESULTS_DIR}"
        return
    fi
    local results_root="${PROJECT_ROOT}/studies/results/${study_name}"
    local run_dir="${results_root}/$(date +%s%N)"
    [[ -n "${suffix}" ]] && run_dir="${run_dir}/${suffix}"
    mkdir -p -- "${run_dir}"
    printf '%s\n' "${run_dir}"
}
