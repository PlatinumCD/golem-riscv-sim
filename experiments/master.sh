#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the complete GPT-2 experiment sweep.

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat >&2 <<EOF
usage: $0 [--no-trace] [--keep-intermediate-mlir] [--force]
  Run the complete configured sweep with SST simulations and traces.

  --no-trace                    Do not record detailed simulation traces.
  --keep-intermediate-mlir      Keep generated compiler MLIR files.
  --force                       Re-run trials that already have completion markers.
  --help                        Show this help text.
EOF
}

trace=true
remove_intermediate_mlir=true
force="${GOLEM_SWEEP_FORCE:-false}"

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --no-trace)
            trace=false
            shift
            ;;
        --keep-intermediate-mlir)
            remove_intermediate_mlir=false
            shift
            ;;
        --force)
            force=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

sweep_options=(--run)
if [[ "${trace}" == true ]]; then
    sweep_options+=(--trace)
fi
if [[ "${remove_intermediate_mlir}" == true ]]; then
    sweep_options+=(--remove-intermediate-mlir)
fi

echo "Starting the complete experiment sweep."
echo "  traces: ${trace}"
echo "  remove intermediate MLIR: ${remove_intermediate_mlir}"
echo "  force completed trials: ${force}"

GOLEM_SWEEP_FORCE="${force}" \
    exec "${SCRIPT_DIR}/sweep.sh" "${sweep_options[@]}"
