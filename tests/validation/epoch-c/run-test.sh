#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly EPOCH_MANIFEST="${PROJECT_ROOT}/config/epoch-c.env"

if [[ ! -f "${EPOCH_MANIFEST}" ]]; then
    echo "missing Epoch C manifest: ${EPOCH_MANIFEST}" >&2
    exit 1
fi

# shellcheck source=../../../config/epoch-c.env
source "${EPOCH_MANIFEST}"

readonly gates=(
    "${CPU_VALIDATION}"
    "${NETWORK_VALIDATION}"
    "${ANALOG_VALIDATION}"
    "${MEMORY_VALIDATION}"
    "${TRANSMIT_VALIDATION}"
)

echo "Epoch ${EPOCH_VERSION}: ${EPOCH_NAME}"
for gate in "${gates[@]}"; do
    echo "[Epoch ${EPOCH_VERSION}] ${gate}"
    "${PROJECT_ROOT}/${gate}"
done

echo "Epoch ${EPOCH_VERSION} component validation: PASS"
