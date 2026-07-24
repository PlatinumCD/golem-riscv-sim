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
readonly SYNC_BRIDGE_TEST="${HOST_TEST_DIR}/sync-bridge-test"
readonly CROSSSIM_TEST="${HOST_TEST_DIR}/crosssim-backend-test"
readonly CROSSSIM_PYTHON="${CROSSSIM_PYTHON:-/usr/bin/python3}"
readonly CROSSSIM_PYTHON_CONFIG="${CROSSSIM_PYTHON_CONFIG:-/usr/bin/python3-config}"
readonly CROSSSIM_SITE_PACKAGES="${INSTALL_ROOT}/cross-sim/python"

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
    -pthread \
    -I"${PROJECT_ROOT}/bridge/include" \
    -I"${PROJECT_ROOT}/components/elements/mittens" \
    "${PROJECT_ROOT}/components/elements/mittens/sharedSyncMemoryBridge.cc" \
    "${TEST_DIR}/sync_bridge_test.cpp" \
    -o "${SYNC_BRIDGE_TEST}"
"${SYNC_BRIDGE_TEST}"

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
