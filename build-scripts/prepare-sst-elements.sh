#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/sst-elements"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/sst-elements"
readonly ELEMENT="${PROJECT_ROOT}/components/elements/mittens"
readonly DESTINATION="${SOURCE}/src/sst/elements/mittens"
readonly BRIDGE_HEADER="${PROJECT_ROOT}/bridge/include/mittens/bridge.h"

for command in find git install; do
    require_command "${command}"
done
require_file "${ELEMENT}/Makefile.am"
require_file "${BRIDGE_HEADER}"

prepare_worktree "${SUBMODULE}" "${SOURCE}" "${SST_ELEMENTS_COMMIT}" \
    "SST Elements"

while IFS= read -r -d '' file; do
    relative="${file#${ELEMENT}/}"
    install -D -m 0644 "${file}" "${DESTINATION}/${relative}"
done < <(find "${ELEMENT}" -type f -print0)
install -D -m 0644 "${BRIDGE_HEADER}" \
    "${DESTINATION}/include/mittens/bridge.h"

# Platform v0 needs only the Merlin network and the Mittens tile element.
while IFS= read -r -d '' directory; do
    element_name="$(basename -- "${directory}")"
    case "${element_name}" in
        merlin|mittens) ;;
        *) touch "${directory}/.ignore" ;;
    esac
done < <(find "${SOURCE}/src/sst/elements" -mindepth 1 -maxdepth 1 \
    -type d -print0)

git -C "${SOURCE}" diff --check
echo "prepared SST Elements source: ${SOURCE}"
