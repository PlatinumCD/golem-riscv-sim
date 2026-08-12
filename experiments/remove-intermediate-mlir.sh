#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 <experiment-output-directory>" >&2
}

if [[ "$#" -ne 1 ]]; then
    usage
    exit 2
fi

readonly OUTPUT_DIR="$1"
readonly COMPILER_DIR="${OUTPUT_DIR}/compiler"
readonly CORE_OBJECT_DIR="${COMPILER_DIR}/cores"

if [[ ! -d "${OUTPUT_DIR}" || -L "${OUTPUT_DIR}" ]]; then
    echo "experiment output directory does not exist: ${OUTPUT_DIR}" >&2
    exit 1
fi
if [[ ! -d "${COMPILER_DIR}" || -L "${COMPILER_DIR}" ]]; then
    echo "compiler output directory does not exist: ${COMPILER_DIR}" >&2
    exit 1
fi
if [[ ! -d "${CORE_OBJECT_DIR}" || -L "${CORE_OBJECT_DIR}" ]]; then
    echo "core object directory does not exist: ${CORE_OBJECT_DIR}" >&2
    exit 1
fi
readonly OUTPUT_REAL_PATH="$(realpath -e -- "${OUTPUT_DIR}")"
readonly COMPILER_REAL_PATH="$(realpath -e -- "${COMPILER_DIR}")"
readonly CORE_OBJECT_REAL_PATH="$(realpath -e -- "${CORE_OBJECT_DIR}")"
case "${COMPILER_REAL_PATH}" in
    "${OUTPUT_REAL_PATH}"/*) ;;
    *)
        echo "compiler output directory is outside the experiment output directory" >&2
        exit 2
        ;;
esac
case "${CORE_OBJECT_REAL_PATH}" in
    "${COMPILER_REAL_PATH}"/*) ;;
    *)
        echo "core object directory is outside the compiler output directory" >&2
        exit 2
        ;;
esac

removed_files=0
removed_bytes=0

remove_file() {
    local path="$1"
    local size

    if [[ ! -f "${path}" ]]; then
        return
    fi
    if [[ -L "${path}" ]]; then
        echo "the script cannot remove a symbolic link: ${path}" >&2
        exit 2
    fi
    size="$(stat -c '%s' -- "${path}")"
    rm -f -- "${path}"
    ((removed_files += 1))
    ((removed_bytes += size))
}

for stage in \
    model.mlir \
    01-canonical.mlir \
    02-converted.mlir \
    03-golem.mlir \
    04-matrices.mlir \
    05-digital-work.mlir \
    06-ra-tree.mlir \
    07-mapping-plan.mlir \
    08-placed.mlir \
    09-tile-deployment.mlir; do
    remove_file "${COMPILER_DIR}/${stage}"
done

while IFS= read -r -d '' path; do
    remove_file "${path}"
done < <(
    find "${CORE_OBJECT_DIR}" -maxdepth 1 -type f -name '*.mlir' -print0
)

echo "removed ${removed_files} intermediate MLIR files (${removed_bytes} bytes)"
echo "all generated MLIR files were removed"
