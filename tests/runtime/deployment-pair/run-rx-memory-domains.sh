#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Both cases receive into ordinary RAM. Enabling SPM must not redirect RAM
# writes to SPM arbitration or subtract the scratchpad base from RAM addresses.
for enabled in false true; do
    echo "ordinary-memory RX with scratchpad_enabled=${enabled}"
    MITTENS_DEPLOYMENT_SCRATCHPAD_ENABLED="${enabled}" \
        bash "${TEST_DIR}/run-test.sh"
done
echo "RX ordinary-memory destination selection: PASS"
