#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sweep-common.sh
source "${SCRIPT_DIR}/sweep-common.sh"

sweep_validate_settings

{
    printf 'name\tschedule\tgreedy heuristic\tbalanced reductions\n'
    sweep_configuration_rows |
        awk -F '\t' 'BEGIN { OFS = "\t" } {
            if ($3 == "none")
                $3 = "-"
            if ($4 == "1")
                $4 = "yes (width 2)"
            else
                $4 = "no"
            print
        }'
} | column -t -s $'\t'

printf '\nStatic token lengths: 4, 8, 16, 32\n'
printf 'Compute modes: analog, digital\n'
printf 'Total deployments: 14 x 4 x 2 = 112\n'
