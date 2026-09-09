#!/usr/bin/env bash
set -euo pipefail
readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec bash "${TEST_DIR}/run-rx-memory-domains.sh"
