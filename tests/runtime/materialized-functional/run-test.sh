#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly ARTIFACT_ROOT="${MITTENS_MATERIALIZED_ARTIFACT_ROOT:-${TEST_RESULTS_ROOT}/materialized-functional/artifacts}"
readonly CASES=(pointwise fork pool concat reduction layout-conversion)

for case_name in "${CASES[@]}"; do
    artifact_dir="${ARTIFACT_ROOT}/${case_name}"
    "${TEST_DIR}/generate-case.sh" "${case_name}" "${artifact_dir}"
    "${TEST_DIR}/run-case.sh" "${case_name}" "${artifact_dir}"
done

python3 "${TEST_DIR}/validate_exact_certificate.py" \
    --opt "${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt" \
    --certificate-module \
        "${ARTIFACT_ROOT}/pointwise/cores/core-0-compute-llvm.mlir" \
    --dependency-module \
        "${ARTIFACT_ROOT}/pool/cores/core-0-compute-llvm.mlir"

"${PROJECT_ROOT}/tests/compiler/exact-ram-readiness/run-test.sh"

# The materialized V1 path is the default, but the validated direct-SPM NoC
# mode remains an optional optimization.  Keep its real generated-ABI QEMU/SST
# regression in the M4 gate so materialization cannot silently break it.
timeout --foreground --signal=TERM --kill-after=10s 300s \
    "${PROJECT_ROOT}/tests/compiler/parametric-noc/run-test.sh" </dev/null

echo "materialized functional M4: PASS (six structural SST cases, exact-readiness corruption, and direct-NoC preservation)"
