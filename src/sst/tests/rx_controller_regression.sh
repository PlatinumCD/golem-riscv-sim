#!/usr/bin/env bash
set -euo pipefail
readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly FIXTURES="${PROJECT_ROOT}/tests/network/transmit-fanout"
# Independent software ownership fixture; deployment DMA cases are opt-in.
bash "${FIXTURES}/run-software-payload-regression.sh"
