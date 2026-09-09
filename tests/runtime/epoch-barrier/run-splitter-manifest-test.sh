#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly SPLITTER="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-split-tile-deployment"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/epoch-barrier/splitter-manifest"
readonly CORE_DIR="${OUTPUT_DIR}/cores"
readonly ACTIVE_MANIFEST="${OUTPUT_DIR}/active-cores.txt"
readonly DEPLOYMENT_MANIFEST="${OUTPUT_DIR}/deployment-manifest.json"

require_executable "${SPLITTER}"
mkdir -p -- "${CORE_DIR}"
find "${CORE_DIR}" -maxdepth 1 -type f -name 'core-*-extracted.mlir' -delete
rm -f -- "${ACTIVE_MANIFEST}" "${DEPLOYMENT_MANIFEST}" \
    "${OUTPUT_DIR}/missing-json.log" \
    "${OUTPUT_DIR}/missing-json-active-cores.txt"

"${SPLITTER}" "${TEST_DIR}/splitter-deployment.mlir" \
    --output-directory="${CORE_DIR}" \
    --manifest="${ACTIVE_MANIFEST}" \
    --deployment-manifest="${DEPLOYMENT_MANIFEST}"

mapfile -t active_tiles <"${ACTIVE_MANIFEST}"
if [[ "${active_tiles[*]}" != "0 2" ]] ||
   [[ ! -s "${CORE_DIR}/core-0-extracted.mlir" ]] ||
   [[ ! -s "${CORE_DIR}/core-2-extracted.mlir" ]]; then
    echo "splitter did not emit the expected active deployment" >&2
    exit 1
fi
for tile in 0 2; do
    extracted="${CORE_DIR}/core-${tile}-extracted.mlir"
    if grep -Fq 'boundary_id = 99 : i64' "${extracted}"; then
        echo "splitter retained an unreferenced deployment boundary on tile ${tile}" >&2
        exit 1
    fi
    grep -F 'sculptor.arch.streaming' "${extracted}" >/dev/null
    grep -F 'sculptor.materialization.boundaries = []' \
        "${extracted}" >/dev/null
    grep -F 'sculptor.materialization.epoch_count = 3 : i64' \
        "${extracted}" >/dev/null
done

python3 - "${PROJECT_ROOT}" "${DEPLOYMENT_MANIFEST}" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "tests" / "support"))
from deployment_manifest import load_deployment_manifest

contract = load_deployment_manifest(
    sys.argv[2], network_size=4, expected_active_tiles=[0, 2]
)
expected = {
    "active_tile_ids": [0, 2],
    "synchronization_mode": "bulk_barrier",
    "epoch_count": 3,
}
if contract != expected:
    raise AssertionError(f"unexpected splitter manifest: {contract}")
PY

if "${SPLITTER}" "${TEST_DIR}/splitter-deployment.mlir" \
    --output-directory="${CORE_DIR}" \
    --manifest="${OUTPUT_DIR}/missing-json-active-cores.txt" \
    >"${OUTPUT_DIR}/missing-json.log" 2>&1; then
    echo "materialized splitter accepted a missing deployment JSON path" >&2
    exit 1
fi
grep -F 'materialized deployment requires --deployment-manifest' \
    "${OUTPUT_DIR}/missing-json.log" >/dev/null

echo "splitter deployment manifest: PASS (strict JSON, participants, epoch count, required output)"
