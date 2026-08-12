#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly HOST_CXX="${HOST_CXX:-c++}"
readonly HOST_TEST_DIR="${BUILD_ROOT}/tests/mittens-element"
readonly ANALOG_TEST="${HOST_TEST_DIR}/analog-device-test"
readonly ANALOG_BRIDGE_TEST="${HOST_TEST_DIR}/analog-bridge-test"
readonly NIC_BRIDGE_TEST="${HOST_TEST_DIR}/nic-bridge-test"
readonly RECEIVE_DMA_ENGINE_TEST="${HOST_TEST_DIR}/receive-dma-engine-test"
readonly SCRATCHPAD_TIMING_MODEL_TEST="${HOST_TEST_DIR}/scratchpad-timing-model-test"
readonly PERFORMANCE_PROFILE_TEST="${HOST_TEST_DIR}/performance-profile-test"
readonly SYNC_BRIDGE_TEST="${HOST_TEST_DIR}/sync-bridge-test"
readonly MEMORY_ACCESS_COALESCER_TEST="${HOST_TEST_DIR}/memory-access-coalescer-test"
readonly CROSSSIM_TEST="${HOST_TEST_DIR}/crosssim-backend-test"
readonly CROSSSIM_PYTHON="${CROSSSIM_PYTHON:-/usr/bin/python3}"
readonly CROSSSIM_PYTHON_CONFIG="${CROSSSIM_PYTHON_CONFIG:-/usr/bin/python3-config}"
readonly CROSSSIM_SITE_PACKAGES="${INSTALL_ROOT}/cross-sim/python"
readonly PROFILE_ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly PROFILE_TEST_RAW_DIRECTORY="${HOST_TEST_DIR}/performance-profile-raw"
readonly PROFILE_TEST_OUTPUT_DIRECTORY="${HOST_TEST_DIR}/performance-profile"
readonly PROFILE_TEST_STATS="${HOST_TEST_DIR}/performance-profile-router-statistics.csv"
readonly WORMHOLE_TEST_OUTPUT="${HOST_TEST_DIR}/wormhole-network.csv"
readonly WORMHOLE_TEST_STATS="${HOST_TEST_DIR}/wormhole-router-statistics.csv"
readonly WORMHOLE_PARALLEL_OUTPUT="${HOST_TEST_DIR}/wormhole-network-parallel.csv"
readonly WORMHOLE_PARALLEL_STATS="${HOST_TEST_DIR}/wormhole-router-statistics-parallel.csv"
readonly WORMHOLE_WIDE_OUTPUT="${HOST_TEST_DIR}/wormhole-network-wide.csv"
readonly WORMHOLE_WIDE_STATS="${HOST_TEST_DIR}/wormhole-router-statistics-wide.csv"

if [[ ! -x "${SST}" || ! -x "${QEMU}" ]] ||
   [[ ! -x "${CROSSSIM_PYTHON}" || ! -x "${CROSSSIM_PYTHON_CONFIG}" ]] ||
   [[ ! -d "${CROSSSIM_SITE_PACKAGES}/simulator" ]] ||
   ! command -v "${HOST_CXX}" >/dev/null 2>&1; then
    echo "build QEMU, SST Core, SST Elements, and CrossSim before running Mittens tests" >&2
    exit 1
fi

mkdir -p -- "${HOST_TEST_DIR}"
"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/bridge/include" \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/analog/analogDevice.cc" \
    "${PROJECT_ROOT}/components/elements/mittens/analog/nativeAnalogBackend.cc" \
    "${TEST_DIR}/analog_device_test.cpp" \
    -o "${ANALOG_TEST}"
"${ANALOG_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/bridge/include" \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/sharedAnalogMemoryBridge.cc" \
    "${TEST_DIR}/analog_bridge_test.cpp" \
    -o "${ANALOG_BRIDGE_TEST}"
"${ANALOG_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/bridge/include" \
    "${TEST_DIR}/nic_bridge_test.cpp" \
    -o "${NIC_BRIDGE_TEST}"
"${NIC_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/receiveDMAEngine.cc" \
    "${TEST_DIR}/receive_dma_engine_test.cpp" \
    -o "${RECEIVE_DMA_ENGINE_TEST}"
"${RECEIVE_DMA_ENGINE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/scratchpad/scratchpadTimingModel.cc" \
    "${TEST_DIR}/scratchpad_timing_model_test.cpp" \
    -o "${SCRATCHPAD_TIMING_MODEL_TEST}"
"${SCRATCHPAD_TIMING_MODEL_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/performanceProfile.cc" \
    "${TEST_DIR}/performance_profile_test.cpp" \
    -o "${PERFORMANCE_PROFILE_TEST}"
"${PERFORMANCE_PROFILE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -pthread \
    -I"${PROJECT_ROOT}/bridge/include" \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/sharedSyncMemoryBridge.cc" \
    "${TEST_DIR}/sync_bridge_test.cpp" \
    -o "${SYNC_BRIDGE_TEST}"
"${SYNC_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/bridge/include" \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${TEST_DIR}/memory_access_coalescer_test.cpp" \
    -o "${MEMORY_ACCESS_COALESCER_TEST}"
"${MEMORY_ACCESS_COALESCER_TEST}"

read -r -a python_cppflags <<< "$("${CROSSSIM_PYTHON_CONFIG}" --includes)"
read -r -a python_ldflags <<< "$("${CROSSSIM_PYTHON_CONFIG}" --embed --ldflags)"
"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${python_cppflags[@]}" \
    "${PROJECT_ROOT}/components/elements/mittens/analog/crossSimAnalogBackend.cc" \
    "${TEST_DIR}/crosssim_backend_test.cpp" \
    "${python_ldflags[@]}" \
    -o "${CROSSSIM_TEST}"
PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}" \
    "${CROSSSIM_TEST}"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" hello
"${PROJECT_ROOT}/build-scripts/build-platform.sh" mesh-pair

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_TEST_ELF="${BUILD_ROOT}/tests/hello/hello.elf"
export MITTENS_TILE0_ELF="${BUILD_ROOT}/tests/mesh-pair/tile0-sender.elf"
export MITTENS_TILE1_ELF="${BUILD_ROOT}/tests/mesh-pair/tile1-receiver.elf"
export PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}"

"${SST}" "${TEST_DIR}/component_lifecycle.py"
"${SST}" "${TEST_DIR}/analog_configuration.py"
"${SST}" "${TEST_DIR}/qemu_boot.py"
MITTENS_TEST_TILES=4 "${SST}" "${TEST_DIR}/multi_tile_boot.py"
"${SST}" "${TEST_DIR}/two_tile_bridge.py"

rm -f -- "${WORMHOLE_TEST_OUTPUT}" "${WORMHOLE_TEST_STATS}"
MITTENS_WORMHOLE_OUTPUT="${WORMHOLE_TEST_OUTPUT}" \
MITTENS_WORMHOLE_STATS="${WORMHOLE_TEST_STATS}" \
    "${SST}" "${TEST_DIR}/wormhole_network.py"
if [[ "$(wc -l <"${WORMHOLE_TEST_OUTPUT}")" -ne 3 ]]; then
    echo "wormhole network test did not record two packets" >&2
    exit 1
fi
if ! grep -q 'flits_forwarded' "${WORMHOLE_TEST_STATS}"; then
    echo "wormhole network test did not record router statistics" >&2
    exit 1
fi
python3 - "${WORMHOLE_TEST_STATS}" <<'PY'
import csv
import sys

with open(sys.argv[1], "r", encoding="utf-8", newline="") as source:
    rows = csv.DictReader(source)
    stalls = sum(
        int(row["Sum.u64"])
        for row in rows
        if row["StatisticName"] == "switch_arbitration_stall_cycles"
    )
assert stalls == 16, stalls
PY

rm -f -- "${WORMHOLE_PARALLEL_OUTPUT}" "${WORMHOLE_PARALLEL_STATS}"
MITTENS_WORMHOLE_OUTPUT="${WORMHOLE_PARALLEL_OUTPUT}" \
MITTENS_WORMHOLE_STATS="${WORMHOLE_PARALLEL_STATS}" \
    "${SST}" -n 2 "${TEST_DIR}/wormhole_network.py"
cmp "${WORMHOLE_TEST_OUTPUT}" "${WORMHOLE_PARALLEL_OUTPUT}"

rm -f -- "${WORMHOLE_WIDE_OUTPUT}" "${WORMHOLE_WIDE_STATS}"
MITTENS_WORMHOLE_OUTPUT="${WORMHOLE_WIDE_OUTPUT}" \
MITTENS_WORMHOLE_STATS="${WORMHOLE_WIDE_STATS}" \
MITTENS_WORMHOLE_LINK_WIDTH_BITS=64 \
    "${SST}" "${TEST_DIR}/wormhole_network.py"
python3 - "${WORMHOLE_WIDE_STATS}" <<'PY'
import csv
import sys

with open(sys.argv[1], "r", encoding="utf-8", newline="") as source:
    rows = csv.DictReader(source)
    stalls = sum(
        int(row["Sum.u64"])
        for row in rows
        if row["StatisticName"] == "switch_arbitration_stall_cycles"
    )
assert stalls == 8, stalls
PY

mkdir -p -- \
    "${PROFILE_TEST_RAW_DIRECTORY}" \
    "${PROFILE_TEST_OUTPUT_DIRECTORY}"
find "${PROFILE_TEST_RAW_DIRECTORY}" \
    -maxdepth 1 \
    -type f \
    -name 'tile-*.csv' \
    -delete
find "${PROFILE_TEST_OUTPUT_DIRECTORY}" \
    -maxdepth 1 \
    -type f \
    \( -name '*.csv' -o -name '*.json' \) \
    -delete
rm -f -- "${PROFILE_TEST_STATS}"
export MITTENS_PROFILE_TEST_RAW_DIRECTORY="${PROFILE_TEST_RAW_DIRECTORY}"
export MITTENS_PROFILE_TEST_STATS="${PROFILE_TEST_STATS}"
"${SST}" "${TEST_DIR}/performance_profile.py"
python3 "${PROFILE_ANALYZER}" \
    "${PROFILE_TEST_RAW_DIRECTORY}" \
    "${PROFILE_TEST_OUTPUT_DIRECTORY}" \
    --router-statistics "${PROFILE_TEST_STATS}"
python3 - \
    "${PROFILE_TEST_OUTPUT_DIRECTORY}/summary.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as source:
    summary = json.load(source)

assert summary["network"]["packets"] == 2
assert summary["network"]["injected_words"] == 2
assert summary["network"]["directional_word_hops"] == 2
assert summary["network"]["physical_router_link_bits"] >= 64
PY
