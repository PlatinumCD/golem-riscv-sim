#!/usr/bin/env bash
set -euo pipefail
readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly FIXTURES="${PROJECT_ROOT}/tests/network/transmit-fanout"
# Existing guests exercise software ownership, delayed cross-source DMA claims,
# and a blocked receive head. Each fixture has its own functional/order checks.
bash "${FIXTURES}/run-software-payload-regression.sh"
bash "${FIXTURES}/run-receive-order-regression.sh"
bash "${FIXTURES}/run-receive-head-blocking-regression.sh"
