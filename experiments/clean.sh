#!/usr/bin/env bash
set -euo pipefail

# Resolve the experiment result directory.

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly RESULTS_DIR="${SCRIPT_DIR}/results"

usage() {
    echo "usage: $0 <experiment-name> [--trial <trial-path>]" >&2
}

# Validate the experiment name.

if [[ "$#" -ne 1 && "$#" -ne 3 ]]; then
    usage
    exit 2
fi

if [[ "$#" -eq 3 && "$2" != --trial ]]; then
    usage
    exit 2
fi

readonly EXPERIMENT_NAME="$1"
if [[ ! "${EXPERIMENT_NAME}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "invalid experiment name: ${EXPERIMENT_NAME}" >&2
    echo "use only letters, numbers, periods, underscores, and hyphens" >&2
    exit 2
fi

readonly EXPERIMENT_DIR="${RESULTS_DIR}/${EXPERIMENT_NAME}"
if [[ ! -d "${EXPERIMENT_DIR}" || -L "${EXPERIMENT_DIR}" ]]; then
    echo "experiment does not exist: ${EXPERIMENT_NAME}" >&2
    exit 1
fi

# Select either the full experiment or one trial beneath it.

DELETE_TARGET="${EXPERIMENT_DIR}"
DELETE_DESCRIPTION="experiment: ${EXPERIMENT_NAME}"
if [[ "$#" -eq 3 ]]; then
    readonly TRIAL_PATH="$3"
    if [[ -z "${TRIAL_PATH}" || "${TRIAL_PATH}" == /* ]]; then
        echo "invalid trial path: ${TRIAL_PATH}" >&2
        exit 2
    fi

    IFS='/' read -r -a trial_parts <<<"${TRIAL_PATH}"
    for part in "${trial_parts[@]}"; do
        if [[ ! "${part}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
            echo "invalid trial path: ${TRIAL_PATH}" >&2
            exit 2
        fi
    done

    DELETE_TARGET="${EXPERIMENT_DIR}/${TRIAL_PATH}"
    if [[ ! -d "${DELETE_TARGET}" || -L "${DELETE_TARGET}" ]]; then
        echo "trial does not exist: ${EXPERIMENT_NAME}/${TRIAL_PATH}" >&2
        exit 1
    fi

    readonly EXPERIMENT_REAL_PATH="$(realpath -e -- "${EXPERIMENT_DIR}")"
    readonly TARGET_REAL_PATH="$(realpath -e -- "${DELETE_TARGET}")"
    case "${TARGET_REAL_PATH}" in
        "${EXPERIMENT_REAL_PATH}"/*) ;;
        *)
            echo "trial path escapes the experiment directory" >&2
            exit 2
            ;;
    esac
    DELETE_DESCRIPTION="trial: ${EXPERIMENT_NAME}/${TRIAL_PATH}"
fi

# Remove the selected directory and every artifact stored beneath it.

rm -rf -- "${DELETE_TARGET}"
echo "deleted ${DELETE_DESCRIPTION}"
