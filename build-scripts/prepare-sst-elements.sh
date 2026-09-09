#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/sst-elements"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/sst-elements"
readonly ELEMENT="${SST_ELEMENT_ROOT}"
readonly DESTINATION="${SOURCE}/src/sst/elements/mittens"
readonly NIC_BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/NICTileBridge.h"
readonly ANALOG_BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/AnalogTileBridge.h"
readonly SYNC_BRIDGE_HEADER="${HARDWARE_ROOT}/bridge/include/mittens/SyncTileBridge.h"
require_owned_comparison_output "${SOURCE}" "${PREPARED_SOURCE_ROOT}"

for command in find git install; do
    require_command "${command}"
done
require_file "${ELEMENT}/Makefile.am"
require_file "${NIC_BRIDGE_HEADER}"
require_file "${ANALOG_BRIDGE_HEADER}"
require_file "${SYNC_BRIDGE_HEADER}"

prepare_worktree "${SUBMODULE}" "${SOURCE}" "${SST_ELEMENTS_COMMIT}" \
    "SST Elements"

while IFS= read -r -d '' file; do
    relative="${file#${ELEMENT}/}"
    install -D -m 0644 "${file}" "${DESTINATION}/${relative}"
done < <(find "${ELEMENT}" -type f -print0)
install -D -m 0644 "${NIC_BRIDGE_HEADER}" \
    "${DESTINATION}/include/mittens/NICTileBridge.h"
install -D -m 0644 "${ANALOG_BRIDGE_HEADER}" \
    "${DESTINATION}/include/mittens/AnalogTileBridge.h"
install -D -m 0644 "${SYNC_BRIDGE_HEADER}" \
    "${DESTINATION}/include/mittens/SyncTileBridge.h"
if [[ "${HARDWARE_TREE}" == src ]]; then
    install -D -m 0644 "${HARDWARE_ROOT}/bridge/include/mittens/MemoryMap.h" \
        "${DESTINATION}/include/mittens/MemoryMap.h"
fi

# Platform v0.1 needs the Merlin network, Mittens tile, and optional
# memHierarchy memory timing element.
while IFS= read -r -d '' directory; do
    element_name="$(basename -- "${directory}")"
    case "${element_name}" in
        memHierarchy|merlin|mittens)
            rm -f -- "${directory}/.ignore"
            ;;
        *) touch "${directory}/.ignore" ;;
    esac
done < <(find "${SOURCE}/src/sst/elements" -mindepth 1 -maxdepth 1 \
    -type d -print0)

git -C "${SOURCE}" diff --check
echo "prepared SST Elements source: ${SOURCE}"
