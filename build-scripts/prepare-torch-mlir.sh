#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/torch-mlir"
readonly PREPARED="${PREPARED_SOURCE_ROOT}/torch-mlir"
readonly PATCH_DIR="${PROJECT_ROOT}/src/patches/torch-mlir"

require_command git
prepare_worktree \
    "${SUBMODULE}" \
    "${PREPARED}" \
    "${TORCH_MLIR_COMMIT}" \
    "Torch-MLIR"
apply_patch_once \
    "${PREPARED}" \
    "${PATCH_DIR}/0001-support-installed-mlir-configuration.patch"

echo "prepared Torch-MLIR source: ${PREPARED}"
