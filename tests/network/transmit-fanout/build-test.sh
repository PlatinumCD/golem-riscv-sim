#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/transmit-fanout"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly TAIL_STRESS_WAVES="${MITTENS_TAIL_STRESS_WAVES:-4}"
readonly TAIL_STRESS_WORDS="${MITTENS_TAIL_STRESS_WORDS:-64}"

software_only="${MITTENS_FANOUT_BUILD_SOFTWARE_PAYLOAD_ONLY:-0}"
runtime_flags=()
runtime_libraries=()
if [[ "${software_only}" != 1 ]]; then
    GOLEM_RUNTIME_ENABLE_PROFILE=1 "${PROJECT_ROOT}/build-scripts/build-runtime.sh"
    runtime_flags+=("-I${RUNTIME_INCLUDE}")
    runtime_libraries+=("${RUNTIME_LIBRARY}")
fi
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
    "${runtime_flags[@]}"
    "-I${PLATFORM_ROOT}"
)

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PLATFORM_STARTUP_ROOT}/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
for source in uart platform-exit freestanding-memory; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        -c "$(platform_source "${source}.cpp")" \
        -o "${OUTPUT_DIR}/${source}.o"
done

# Receive complete framed payloads through the software word interface,
# without registering an RX-DMA descriptor.
for tile_id in 0 1; do
    object="${OUTPUT_DIR}/software-payload-2-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/software-payload-2-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=1 \
        -DMITTENS_FANOUT_SOURCE_COUNT=1 \
        -DMITTENS_FANOUT_DESTINATION_TILE=1 \
        -DMITTENS_FANOUT_SOFTWARE_PAYLOAD=1 \
        -DMITTENS_FANOUT_SOFTWARE_PAYLOAD_WORDS=1022 \
        -DMITTENS_FANOUT_RECEIVER_DELAY_CYCLES=500000 \
        -c "${PROJECT_ROOT}/src/sst/tests/rx-software-payload.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${runtime_libraries[@]}" \
        -o "${elf}"
done

if [[ "${MITTENS_FANOUT_BUILD_SOFTWARE_PAYLOAD_ONLY:-0}" == "1" ]]; then
    echo "built software-payload ELFs in ${OUTPUT_DIR}"
    exit 0
fi

for tile_id in $(seq 0 31); do
    object="${OUTPUT_DIR}/tail-bidirectional-32-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/tail-bidirectional-32-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=2 \
        -DMITTENS_FANOUT_ELEMENT_COUNT=768 \
        -DMITTENS_FANOUT_SOURCE_COUNT=32 \
        -DMITTENS_FANOUT_SHARED_RESOURCE_ID=1 \
        -DMITTENS_FANOUT_BIDIRECTIONAL=1 \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

for tile_id in $(seq 0 31); do
    object="${OUTPUT_DIR}/tail-sustained-${TAIL_STRESS_WAVES}-${TAIL_STRESS_WORDS}-32-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/tail-sustained-${TAIL_STRESS_WAVES}-${TAIL_STRESS_WORDS}-32-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=2 \
        "-DMITTENS_FANOUT_ELEMENT_COUNT=${TAIL_STRESS_WORDS}" \
        -DMITTENS_FANOUT_SOURCE_COUNT=32 \
        -DMITTENS_FANOUT_SHARED_RESOURCE_ID=1 \
        -DMITTENS_FANOUT_BIDIRECTIONAL=1 \
        "-DMITTENS_FANOUT_WAVES=${TAIL_STRESS_WAVES}" \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

for fanout in 1 2 4 8 16 24; do
    for tile_id in 0 1; do
        object="${OUTPUT_DIR}/fanout-${fanout}-tile${tile_id}.o"
        elf="${OUTPUT_DIR}/fanout-${fanout}-tile${tile_id}.elf"
        "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
            "-DMITTENS_TILE_ID=${tile_id}" \
            "-DMITTENS_FANOUT=${fanout}" \
            -c "${TEST_DIR}/main.cpp" \
            -o "${object}"
        "${LLVM}/bin/clang++" "${common_flags[@]}" \
            -nostdlib -nostartfiles -nodefaultlibs \
            -fuse-ld=lld \
            -Wl,--build-id=none \
            -Wl,--gc-sections \
            "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
            "${OUTPUT_DIR}/crt0.o" \
            "${OUTPUT_DIR}/uart.o" \
            "${OUTPUT_DIR}/platform-exit.o" \
            "${OUTPUT_DIR}/freestanding-memory.o" \
            "${object}" \
            "${RUNTIME_LIBRARY}" \
            -o "${elf}"
    done
    for policy in blocking async; do
        for tile_id in 0 1; do
            object="${OUTPUT_DIR}/overlap-${policy}-${fanout}-tile${tile_id}.o"
            elf="${OUTPUT_DIR}/overlap-${policy}-${fanout}-tile${tile_id}.elf"
            policy_flags=(
                "-DMITTENS_FANOUT_INDEPENDENT_TASKS=1"
            )
            if [[ "${policy}" == "async" ]]; then
                policy_flags+=(
                    "-DMITTENS_RUNTIME_ASYNC_TRANSMIT=1"
                )
            fi
            "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
                "${policy_flags[@]}" \
                "-DMITTENS_TILE_ID=${tile_id}" \
                "-DMITTENS_FANOUT=${fanout}" \
                -c "${TEST_DIR}/main.cpp" \
                -o "${object}"
            "${LLVM}/bin/clang++" "${common_flags[@]}" \
                -nostdlib -nostartfiles -nodefaultlibs \
                -fuse-ld=lld \
                -Wl,--build-id=none \
                -Wl,--gc-sections \
                "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
                "${OUTPUT_DIR}/crt0.o" \
                "${OUTPUT_DIR}/uart.o" \
                "${OUTPUT_DIR}/platform-exit.o" \
                "${OUTPUT_DIR}/freestanding-memory.o" \
                "${object}" \
                "${RUNTIME_LIBRARY}" \
                -o "${elf}"
        done
    done
done

# Hold the destination out of its receive loop until frames from two sources
# have converged on the SST tile.  This deterministically exercises global
# bridge admission while preserving each source's header/payload order.
for tile_id in 0 1 2; do
    object="${OUTPUT_DIR}/receive-order-2-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/receive-order-2-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=8 \
        -DMITTENS_FANOUT_ELEMENT_COUNT=1 \
        -DMITTENS_FANOUT_SOURCE_COUNT=2 \
        -DMITTENS_FANOUT_DESTINATION_TILE=2 \
        -DMITTENS_FANOUT_INDEPENDENT_TASKS=1 \
        -DMITTENS_FANOUT_RECEIVER_DELAY_CYCLES=250000 \
        -DMITTENS_FANOUT_FIRST_DMA_DELAY_CYCLES=500000 \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

# Fill every receive-bridge burst slot from source zero, then queue source-one
# headers before the destination submits its first RX DMA.  The earlier
# source-zero payload must be admitted ahead of those queued later headers.
for tile_id in 0 1 2; do
    object="${OUTPUT_DIR}/receive-head-blocking-2-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/receive-head-blocking-2-tile${tile_id}.elf"
    source_delay_cycles=0
    if ((tile_id == 1)); then
        source_delay_cycles=200000
    fi
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=8 \
        -DMITTENS_FANOUT_ELEMENT_COUNT=16 \
        -DMITTENS_FANOUT_SOURCE_COUNT=2 \
        -DMITTENS_FANOUT_DESTINATION_TILE=2 \
        -DMITTENS_FANOUT_INDEPENDENT_TASKS=1 \
        "-DMITTENS_FANOUT_SOURCE_DELAY_CYCLES=${source_delay_cycles}" \
        -DMITTENS_FANOUT_RECEIVER_DELAY_CYCLES=500000 \
        -DMITTENS_FANOUT_FIRST_DMA_DELAY_CYCLES=500000 \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

# Reproduce the GPT-2 route-tail failure with two 768-word routes from one
# source task.  Keep these ELFs separate from the original 512-word sweep.
for tile_id in 0 1; do
    object="${OUTPUT_DIR}/tail-regression-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/tail-regression-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=2 \
        -DMITTENS_FANOUT_ELEMENT_COUNT=768 \
        -DMITTENS_FANOUT_SHARED_RESOURCE_ID=1 \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

tail_contention_sources="${MITTENS_TAIL_CONTENTION_SOURCES:-8}"
if [[ ! "${tail_contention_sources}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_TAIL_CONTENTION_SOURCES must be positive" >&2
    exit 2
fi
for tile_id in $(seq 0 "${tail_contention_sources}"); do
    object="${OUTPUT_DIR}/tail-contention-${tail_contention_sources}-tile${tile_id}.o"
    elf="${OUTPUT_DIR}/tail-contention-${tail_contention_sources}-tile${tile_id}.elf"
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        "-DMITTENS_TILE_ID=${tile_id}" \
        -DMITTENS_FANOUT=2 \
        -DMITTENS_FANOUT_ELEMENT_COUNT=768 \
        "-DMITTENS_FANOUT_SOURCE_COUNT=${tail_contention_sources}" \
        "-DMITTENS_FANOUT_DESTINATION_TILE=${tail_contention_sources}" \
        -DMITTENS_FANOUT_SHARED_RESOURCE_ID=1 \
        -c "${TEST_DIR}/main.cpp" \
        -o "${object}"
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${object}" \
        "${RUNTIME_LIBRARY}" \
        -o "${elf}"
done

echo "built transmit fan-out ELFs in ${OUTPUT_DIR}"
