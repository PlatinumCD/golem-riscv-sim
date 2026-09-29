#!/usr/bin/env bash
set -euo pipefail
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"
readonly SUBMODULE="${PROJECT_ROOT}/third_party/sst-elements"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/sst-elements"
require_owned_comparison_output "${SOURCE}" "${PREPARED_SOURCE_ROOT}"
prepare_worktree "${SUBMODULE}" "${SOURCE}" "${SST_ELEMENTS_COMMIT}" 'SST Elements'
apply_patch_once "${SOURCE}" "${PROJECT_ROOT}/src/patches/sst-elements/mesh-single-vc.patch"
# Project components are built separately. Shared dependencies contain no tile model.
while IFS= read -r -d '' directory; do
    case "$(basename -- "${directory}")" in
        memHierarchy|merlin) rm -f -- "${directory}/.ignore" ;;
        *) touch "${directory}/.ignore" ;;
    esac
done < <(find "${SOURCE}/src/sst/elements" -mindepth 1 -maxdepth 1 -type d -print0)
echo "prepared shared SST Elements: ${SOURCE}"
