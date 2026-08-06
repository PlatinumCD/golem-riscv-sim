#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=factorial-common.sh
source "${SCRIPT_DIR}/factorial-common.sh"

factorial_validate_settings
printf 'name\tboundary_regret\tcompact_region\tspatial_link_pressure\tbalanced_reductions\tdistributed_matmul\tgreedy_heuristic\n'
factorial_configuration_rows
