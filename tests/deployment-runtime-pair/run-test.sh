#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/deployment-runtime-pair"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly SIMULATION_OUTPUT="${OUTPUT_DIR}/simulation.log"
readonly STATISTICS="${OUTPUT_DIR}/router-statistics.csv"
readonly RX_DMA_WIDTH_BITS="${MITTENS_DEPLOYMENT_RX_DMA_WIDTH_BITS:-256}"
readonly RX_DMA_SETUP_CYCLES="${MITTENS_DEPLOYMENT_RX_DMA_SETUP_CYCLES:-8}"
readonly NETWORK_PACKET_WORDS=16
readonly ROUTE_WORDS=300

if [[ ! "${RX_DMA_WIDTH_BITS}" =~ ^[1-9][0-9]*$ ]] ||
   (( RX_DMA_WIDTH_BITS % 32 != 0 )); then
    echo "MITTENS_DEPLOYMENT_RX_DMA_WIDTH_BITS must be a positive multiple of 32" >&2
    exit 1
fi
if [[ ! "${RX_DMA_SETUP_CYCLES}" =~ ^[0-9]+$ ]]; then
    echo "MITTENS_DEPLOYMENT_RX_DMA_SETUP_CYCLES must be a nonnegative integer" >&2
    exit 1
fi
readonly RX_DMA_WORDS_PER_CYCLE=$((RX_DMA_WIDTH_BITS / 32))
readonly RX_DMA_EXPECTED_CYCLES=$((
    (ROUTE_WORDS + RX_DMA_WORDS_PER_CYCLE - 1) / RX_DMA_WORDS_PER_CYCLE +
    RX_DMA_SETUP_CYCLES
))
readonly RX_DMA_EXPECTED_TRANSFERS=$((
    (ROUTE_WORDS + NETWORK_PACKET_WORDS - 1) / NETWORK_PACKET_WORDS
))

for executable in \
    "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" \
    "${LLVM}/bin/llvm-readelf" \
    "${QEMU}" \
    "${SST}"; do
    require_executable "${executable}"
done

"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
mkdir -p -- "${OUTPUT_DIR}"

common_flags=(
    "--target=${GOLEM_TARGET}"
    "-mcpu=${GOLEM_CPU}"
    "-mabi=${GOLEM_ABI}"
    -mcmodel=medany
    -ffreestanding
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    -g
    -O2
)
cxx_flags=(
    "${common_flags[@]}"
    -std=c++20
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    "-I${RUNTIME_INCLUDE}"
    "-I${PROJECT_ROOT}/platform"
)

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
for source in uart platform-exit freestanding-memory; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/${source}.cpp" \
        -o "${OUTPUT_DIR}/${source}.o"
done

for tile_id in 0 1; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -c "${TEST_DIR}/main.cpp" \
        -o "${OUTPUT_DIR}/tile${tile_id}-main.o"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${OUTPUT_DIR}/tile${tile_id}-main.o" \
        "${RUNTIME_LIBRARY}" \
        -o "${OUTPUT_DIR}/tile${tile_id}.elf"
done

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_DEPLOYMENT_TILE0_ELF="${OUTPUT_DIR}/tile0.elf"
export MITTENS_DEPLOYMENT_TILE1_ELF="${OUTPUT_DIR}/tile1.elf"
export MITTENS_DEPLOYMENT_STATS="${STATISTICS}"

rm -f -- "${SIMULATION_OUTPUT}" "${STATISTICS}"
"${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"
grep -F "DEPLOYMENT_RUNTIME_TILE_0_PASS" "${SIMULATION_OUTPUT}" >/dev/null
grep -F "DEPLOYMENT_RUNTIME_TILE_1_PASS words=300" \
    "${SIMULATION_OUTPUT}" >/dev/null
grep -E "MITTENS_PROFILE tile=1 .*stop_nic_rx=[1-9][0-9]*" \
    "${SIMULATION_OUTPUT}" >/dev/null || {
    echo "destination tile never entered the blocking NIC receive state" >&2
    exit 1
}
grep -E "MITTENS_PROFILE tile=1 .*stop_nic_rx_dma_submit=1 .*rx_dma_transfers=${RX_DMA_EXPECTED_TRANSFERS} rx_dma_words=${ROUTE_WORDS} rx_dma_active_cycles=${RX_DMA_EXPECTED_CYCLES}" \
    "${SIMULATION_OUTPUT}" >/dev/null || {
    echo "destination tile did not report the expected timed RX DMA" >&2
    exit 1
}
if [[ "${MITTENS_DEPLOYMENT_MEMORY_TOPOLOGY:-private_l1}" != \
      "shared_l2" ]]; then
    dma_observation="$(
        awk '
            /scheduled RX DMA burst/ &&
            match($0, /start=([0-9]+), complete=([0-9]+)/, values) {
                ++transfers
                cycles += values[2] - values[1]
            }
            END {
                print transfers "," cycles
            }
        ' "${SIMULATION_OUTPUT}"
    )"
    if [[ "${dma_observation}" != \
          "${RX_DMA_EXPECTED_TRANSFERS},${RX_DMA_EXPECTED_CYCLES}" ]]; then
        echo "fragmented RX DMA did not retain one setup charge and ${RX_DMA_EXPECTED_CYCLES} total cycles" >&2
        exit 1
    fi
fi
awk -F, '
    $1 == "router_1_0" &&
    $2 == "send_packet_count" &&
    $3 == "port4" {
        packets = $7
    }
    $1 == "router_1_0" &&
    $2 == "send_bit_count" &&
    $3 == "port4" {
        bits = $7
    }
    END {
        exit !(packets == 20 && bits == 9760)
    }
' "${STATISTICS}" || {
    echo "expected one header packet and 19 bounded payload packets (9760 bits)" >&2
    exit 1
}

echo "timed 300-word DeploymentRuntime RX DMA over QEMU + SST: PASS (${RX_DMA_EXPECTED_CYCLES} cycles)"
