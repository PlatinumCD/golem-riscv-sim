#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMPONENT_PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
export GOLEM_HARDWARE_TREE=src
source "${COMPONENT_PROJECT_ROOT}/build-scripts/common.sh"
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
readonly QEMU_READY_SET_EXECUTOR_TEST="${HOST_TEST_DIR}/qemu-ready-set-executor-test"
readonly MEMORY_ACCESS_COALESCER_TEST="${HOST_TEST_DIR}/memory-access-coalescer-test"
readonly GLOBAL_RAM_READINESS_TEST="${HOST_TEST_DIR}/global-ram-readiness-test"
readonly CROSSSIM_TEST="${HOST_TEST_DIR}/crosssim-backend-test"
readonly CROSSSIM_PYTHON="${CROSSSIM_PYTHON:-/usr/bin/python3}"
readonly CROSSSIM_PYTHON_CONFIG="${CROSSSIM_PYTHON_CONFIG:-/usr/bin/python3-config}"
readonly PROFILE_ANALYZER="${PROJECT_ROOT}/tools/analysis/analyze-performance-profile.py"
readonly PROFILE_TEST_RAW_DIRECTORY="${HOST_TEST_DIR}/performance-profile-raw"
readonly PROFILE_TEST_OUTPUT_DIRECTORY="${HOST_TEST_DIR}/performance-profile"
readonly PROFILE_TEST_STATS="${HOST_TEST_DIR}/performance-profile-router-statistics.csv"
readonly WORMHOLE_TEST_OUTPUT="${HOST_TEST_DIR}/wormhole-network.csv"
readonly WORMHOLE_TEST_STATS="${HOST_TEST_DIR}/wormhole-router-statistics.csv"
readonly WORMHOLE_PARALLEL_OUTPUT="${HOST_TEST_DIR}/wormhole-network-parallel.csv"
readonly WORMHOLE_PARALLEL_STATS="${HOST_TEST_DIR}/wormhole-router-statistics-parallel.csv"
readonly WORMHOLE_WIDE_OUTPUT="${HOST_TEST_DIR}/wormhole-network-wide.csv"
readonly WORMHOLE_WIDE_STATS="${HOST_TEST_DIR}/wormhole-router-statistics-wide.csv"
readonly WORMHOLE_CREDIT_OUTPUT="${HOST_TEST_DIR}/wormhole-network-credit-stall.csv"
readonly WORMHOLE_CREDIT_STATS="${HOST_TEST_DIR}/wormhole-router-statistics-credit-stall.csv"

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
    -I"${PROJECT_ROOT}/src/bridge/include" \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/analog/analogDevice.cc" \
    "${PROJECT_ROOT}/src/sst/analog/nativeAnalogBackend.cc" \
    "${PROJECT_ROOT}/src/sst/analog/timingAnalogBackend.cc" \
    "${TEST_DIR}/analog_device_test.cpp" \
    -o "${ANALOG_TEST}"
"${ANALOG_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/bridge/sharedAnalogMemoryBridge.cc" \
    "${TEST_DIR}/analog_bridge_test.cpp" \
    -o "${ANALOG_BRIDGE_TEST}"
"${ANALOG_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    "${TEST_DIR}/nic_bridge_test.cpp" \
    -o "${NIC_BRIDGE_TEST}"
"${NIC_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/network/receiveDMAEngine.cc" \
    "${TEST_DIR}/receive_dma_engine_test.cpp" \
    -o "${RECEIVE_DMA_ENGINE_TEST}"
"${RECEIVE_DMA_ENGINE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/memory/scratchpad/scratchpadTimingModel.cc" \
    "${TEST_DIR}/scratchpad_timing_model_test.cpp" \
    -o "${SCRATCHPAD_TIMING_MODEL_TEST}"
"${SCRATCHPAD_TIMING_MODEL_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/profiling/performanceProfile.cc" \
    "${PROJECT_ROOT}/src/sst/profiling/measurementWriter.cc" \
    "${TEST_DIR}/performance_profile_test.cpp" \
    -o "${PERFORMANCE_PROFILE_TEST}"
"${PERFORMANCE_PROFILE_TEST}"

# Snapshot construction/formatting is independent of SST runtime and headers.
"${HOST_CXX}" -std=c++17 -Wall -Wextra -Werror \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    "${TEST_DIR}/tile_progress_test.cpp" \
    "${PROJECT_ROOT}/src/sst/profiling/tileProgress.cc" \
    "${PROJECT_ROOT}/src/sst/profiling/performanceProfile.cc" \
    "${PROJECT_ROOT}/src/sst/profiling/measurementWriter.cc" \
    -o "${HOST_TEST_DIR}/tile-progress-test"
"${HOST_TEST_DIR}/tile-progress-test"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -pthread \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/bridge/sharedSyncMemoryBridge.cc" \
    "${TEST_DIR}/sync_bridge_test.cpp" \
    -o "${SYNC_BRIDGE_TEST}"
"${SYNC_BRIDGE_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -pthread \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${PROJECT_ROOT}/src/sst/bridge/sharedSyncMemoryBridge.cc" \
    "${PROJECT_ROOT}/src/sst/execution/qemuReadySetExecutor.cc" \
    "${TEST_DIR}/qemu_ready_set_executor_test.cpp" \
    -o "${QEMU_READY_SET_EXECUTOR_TEST}"
"${QEMU_READY_SET_EXECUTOR_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/bridge/include" \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${TEST_DIR}/memory_access_coalescer_test.cpp" \
    -o "${MEMORY_ACCESS_COALESCER_TEST}"
"${MEMORY_ACCESS_COALESCER_TEST}"

"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${TEST_DIR}/global_ram_readiness_test.cpp" \
    -o "${GLOBAL_RAM_READINESS_TEST}"
"${GLOBAL_RAM_READINESS_TEST}"
"${TEST_DIR}/run-global-ram-readiness-test.sh"
python3 -B "${TEST_DIR}/run-injection-order-test.py"

read -r -a python_cppflags <<< "$("${CROSSSIM_PYTHON_CONFIG}" --includes)"
read -r -a python_ldflags <<< "$("${CROSSSIM_PYTHON_CONFIG}" --embed --ldflags)"
"${HOST_CXX}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"${PROJECT_ROOT}/src/sst" -I"${PROJECT_ROOT}/src/bridge/include" \
    "${python_cppflags[@]}" \
    "${PROJECT_ROOT}/src/sst/analog/crossSimAnalogBackend.cc" \
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

# Force both NIC- and router-side credit starvation.  The receiver holds its
# first completed packet until cycle 300, while each sender deliberately uses
# only two of the attached router's credits.  Event-driven clock gating must
# preserve the cycle-driven reference's delivery ticks and every counter sum.
MITTENS_WORMHOLE_OUTPUT="${WORMHOLE_CREDIT_OUTPUT}" \
MITTENS_WORMHOLE_STATS="${WORMHOLE_CREDIT_STATS}" \
MITTENS_WORMHOLE_BUFFER_FLITS=64 \
MITTENS_WORMHOLE_SOURCE_ROUTER_CREDITS=2 \
MITTENS_WORMHOLE_PAYLOAD_WORDS=48 \
MITTENS_WORMHOLE_RECEIVE_CYCLE=300 \
    "${SST}" "${TEST_DIR}/wormhole_network.py"
python3 - "${WORMHOLE_CREDIT_OUTPUT}" "${WORMHOLE_CREDIT_STATS}" <<'PY'
import csv
import sys

with open(sys.argv[1], "r", encoding="utf-8", newline="") as source:
    receipts = list(csv.DictReader(source))
assert receipts == [
    {
        "source": "1",
        "destination": "3",
        "payload_words": "48",
        "injection_tick": "100000",
        "head_arrival_tick": "334000",
        "completion_tick": "334000",
        "latency_ticks": "234000",
    },
    {
        "source": "2",
        "destination": "3",
        "payload_words": "48",
        "injection_tick": "100000",
        "head_arrival_tick": "300000",
        "completion_tick": "300000",
        "latency_ticks": "200000",
    },
], receipts

with open(sys.argv[2], "r", encoding="utf-8", newline="") as source:
    rows = csv.DictReader(source)
    sums = {}
    for row in rows:
        name = row["StatisticName"]
        sums[name] = sums.get(name, 0) + int(row["Sum.u64"])
assert sums == {
    "flits_forwarded": 192,
    "packets_forwarded": 4,
    "input_buffer_full_cycles": 0,
    "switch_arbitration_stall_cycles": 92,
    "output_credit_stall_cycles": 86,
    "output_link_busy_cycles": 192,
    "cardinal_outputs_active": 96,
    "input_buffer_occupancy": 6364,
    "injected_flits": 96,
    "received_flits": 96,
    "completed_packets": 2,
    "injection_credit_stall_cycles": 96,
    "injection_queue_occupancy": 4744,
    "packet_network_latency": 334000,
}, sums
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
