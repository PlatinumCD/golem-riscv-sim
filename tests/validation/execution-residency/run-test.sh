#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly VALIDATOR="${PROJECT_ROOT}/scripts/validate-sculptor-execution-residency.py"
readonly GENERATOR="${TEST_DIR}/make-fixture.py"
readonly OUTPUT="${TEST_RESULTS_ROOT}/validation/execution-residency"

require_executable "${VALIDATOR}"
require_command python3
mkdir -p -- "${OUTPUT}"

validate_fixture() {
    local root="$1"
    "${VALIDATOR}" \
        --audit "${root}/candidate/execution-residency-audit.json" \
        --input-mlir "${root}/candidate/deployment/04-tensor-fragments.mlir" \
        --control-mlir "${root}/control/deployment/04-residency-regions.mlir" \
        --output-mlir "${root}/candidate/deployment/04-residency-regions.mlir" \
        --expected-mode analyze \
        --expected-maximum-members 8 \
        --expected-maximum-wave-width 2 \
        --expected-require-positive-benefit true \
        --control-run-directory "${root}/control" \
        --candidate-run-directory "${root}/candidate" \
        --control-evidence-directory "${root}/off-1" \
        --control-evidence-directory "${root}/off-2" \
        --control-evidence-directory "${root}/off-3" \
        --candidate-evidence-directory "${root}/analyze-1" \
        --candidate-evidence-directory "${root}/analyze-2" \
        --candidate-evidence-directory "${root}/analyze-3"
}

valid_root="${OUTPUT}/valid"
python3 "${GENERATOR}" "${PROJECT_ROOT}" "${valid_root}" none
validate_fixture "${valid_root}" >/dev/null

for kind in binary launch_hash termination physical pending simulated_time; do
    root="${OUTPUT}/${kind}"
    log="${OUTPUT}/${kind}.log"
    python3 "${GENERATOR}" "${PROJECT_ROOT}" "${root}" "${kind}"
    if validate_fixture "${root}" >"${log}" 2>&1; then
        echo "execution-residency validator accepted ${kind} corruption" >&2
        exit 1
    fi
done

validate_phase2_compile_fixture() {
    local root="$1"
    "${VALIDATOR}" \
        --audit "${root}/candidate/execution-residency-audit.json" \
        --input-mlir "${root}/candidate/deployment/04-tensor-fragments.mlir" \
        --output-mlir "${root}/candidate/deployment/04-residency-regions.mlir" \
        --expected-mode analyze \
        --expected-maximum-members 8 \
        --expected-maximum-wave-width 2 \
        --expected-require-positive-benefit true \
        --control-run-directory "${root}/control" \
        --candidate-run-directory "${root}/candidate" \
        --compile-only-differential
}

phase2_root="${OUTPUT}/phase2-valid"
python3 "${GENERATOR}" "${PROJECT_ROOT}" "${phase2_root}" none \
    --candidate-phase 2
validate_phase2_compile_fixture "${phase2_root}" >/dev/null

phase2_corrupt_root="${OUTPUT}/phase2-placement"
phase2_corrupt_log="${OUTPUT}/phase2-placement.log"
python3 "${GENERATOR}" "${PROJECT_ROOT}" "${phase2_corrupt_root}" placement \
    --candidate-phase 2
if validate_phase2_compile_fixture "${phase2_corrupt_root}" \
    >"${phase2_corrupt_log}" 2>&1; then
    echo "execution-residency validator accepted Phase-2 placement drift" >&2
    exit 1
fi

echo "execution-residency Phase-0 SST and Phase-2 compile differential validation: PASS"
