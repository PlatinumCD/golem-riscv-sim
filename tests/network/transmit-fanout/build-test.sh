#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/transmit-fanout"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"

GOLEM_RUNTIME_ENABLE_PROFILE=1 \
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
            "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
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
                "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
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

echo "built transmit fan-out ELFs in ${OUTPUT_DIR}"
