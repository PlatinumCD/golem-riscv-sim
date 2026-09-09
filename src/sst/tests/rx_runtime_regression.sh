#!/usr/bin/env bash
set -euo pipefail
readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
bash "${PROJECT_ROOT}/tests/network/transmit-fanout/run-receive-order-regression.sh"
bash "${PROJECT_ROOT}/tests/network/transmit-fanout/run-receive-head-blocking-regression.sh"
