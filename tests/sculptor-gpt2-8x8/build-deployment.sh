#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly LOWERING_DIR="${MITTENS_GPT2_LOWERING_DIR:-${BUILD_ROOT}/tests/sculptor-gpt2-8x8/fixture-lowering/full}"
readonly COMPILER_ARTIFACTS="${LOWERING_DIR}/cores"
readonly SOURCE_CORE_MANIFEST="${LOWERING_DIR}/active-cores.txt"
readonly OUTPUT_DIR="${MITTENS_GPT2_DEPLOYMENT_DIR:-${BUILD_ROOT}/tests/sculptor-gpt2-8x8/deployment}"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly LINKER_SCRIPT="${TEST_DIR}/tile-64m.ld"
readonly ACTIVE_CORE_MANIFEST="${OUTPUT_DIR}/active-cores.txt"
readonly SEQUENCE_LENGTH="${MITTENS_GPT2_SEQUENCE_LENGTH:-4}"
readonly HIDDEN_SIZE="${MITTENS_GPT2_HIDDEN_SIZE:-768}"
readonly MESH_WIDTH="${MITTENS_GPT2_MESH_WIDTH:-8}"
readonly MESH_HEIGHT="${MITTENS_GPT2_MESH_HEIGHT:-8}"
readonly PROFILE_MODE="${MITTENS_GPT2_PROFILE_MODE:-off}"
readonly TRANSMIT_POLICY="${MITTENS_GPT2_TRANSMIT_POLICY:-blocking}"
readonly SCRATCHPAD_BYTES="${MITTENS_GPT2_SCRATCHPAD_BYTES:-262144}"
readonly ROUTE_EXTRACTOR="${PROJECT_ROOT}/scripts/extract-deployment-routes.py"
readonly ROUTE_MANIFEST="${OUTPUT_DIR}/deployment-routes.csv"

for executable in \
    "${LLVM}/bin/clang" \
    "${LLVM}/bin/clang++" \
    "${LLVM}/bin/llvm-nm" \
    "${LLVM}/bin/llvm-readelf"; do
    require_executable "${executable}"
done
require_executable "${ROUTE_EXTRACTOR}"
if [[ ! "${SEQUENCE_LENGTH}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_SEQUENCE_LENGTH must be a positive integer" >&2
    exit 1
fi
if [[ ! "${HIDDEN_SIZE}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_HIDDEN_SIZE must be a positive integer" >&2
    exit 1
fi
if [[ ! "${MESH_WIDTH}" =~ ^[1-9][0-9]*$ ||
      ! "${MESH_HEIGHT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "GPT-2 mesh dimensions must be positive integers" >&2
    exit 1
fi
readonly NETWORK_SIZE=$((MESH_WIDTH * MESH_HEIGHT))
case "${PROFILE_MODE}" in
    off|summary|trace) ;;
    *)
        echo "MITTENS_GPT2_PROFILE_MODE must be off, summary, or trace" >&2
        exit 1
        ;;
esac
case "${TRANSMIT_POLICY}" in
    blocking|async) ;;
    *)
        echo "MITTENS_GPT2_TRANSMIT_POLICY must be blocking or async" >&2
        exit 1
        ;;
esac
require_file "${SOURCE_CORE_MANIFEST}"
mapfile -t ACTIVE_CORE_IDS <"${SOURCE_CORE_MANIFEST}"
if [[ "${#ACTIVE_CORE_IDS[@]}" -lt 1 ||
      "${#ACTIVE_CORE_IDS[@]}" -gt "${NETWORK_SIZE}" ]]; then
    echo "expected 1 to ${NETWORK_SIZE} active GPT-2 cores, found ${#ACTIVE_CORE_IDS[@]}" >&2
    exit 1
fi
for core_id in "${ACTIVE_CORE_IDS[@]}"; do
    if [[ ! "${core_id}" =~ ^[0-9]+$ ||
          "${core_id}" -ge "${NETWORK_SIZE}" ]]; then
        echo "invalid GPT-2 active core ID: ${core_id}" >&2
        exit 1
    fi
    require_file "${COMPILER_ARTIFACTS}/core-${core_id}.o"
done

"${PROJECT_ROOT}/build-scripts/build-runtime.sh"
require_file "${RUNTIME_LIBRARY}"
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
    -O3
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
    "-DMITTENS_GPT2_SEQUENCE_LENGTH=${SEQUENCE_LENGTH}"
    "-DMITTENS_GPT2_HIDDEN_SIZE=${HIDDEN_SIZE}"
    "-DMITTENS_GPT2_SCRATCHPAD_BYTES=${SCRATCHPAD_BYTES}"
    "-I${RUNTIME_INCLUDE}"
    "-I${PROJECT_ROOT}/platform"
)
if [[ "${PROFILE_MODE}" == "trace" ]]; then
    cxx_flags+=("-DMITTENS_GPT2_TASK_TRACE=1")
fi
if [[ "${TRANSMIT_POLICY}" == "async" ]]; then
    cxx_flags+=("-DMITTENS_GPT2_ASYNC_TRANSMIT=1")
fi

"${LLVM}/bin/clang" "${common_flags[@]}" \
    -c "${PROJECT_ROOT}/platform/crt0.S" \
    -o "${OUTPUT_DIR}/crt0.o"
for source in \
    uart \
    platform-exit \
    freestanding-memory \
    mlir-runtime; do
    "${LLVM}/bin/clang++" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/${source}.cpp" \
        -o "${OUTPUT_DIR}/${source}.o"
done
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -fno-builtin-expf \
    -fno-builtin-erff \
    -c "${TEST_DIR}/freestanding-math.cpp" \
    -o "${OUTPUT_DIR}/freestanding-math.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/tile-main.cpp" \
    -o "${OUTPUT_DIR}/tile-main.o"
"${LLVM}/bin/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/idle-main.cpp" \
    -o "${OUTPUT_DIR}/idle-main.o"

link_elf() {
    local output="$1"
    shift
    "${LLVM}/bin/clang++" "${common_flags[@]}" \
        -nostdlib -nostartfiles -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${LINKER_SCRIPT}" \
        "$@" \
        -o "${output}"
}

link_elf \
    "${OUTPUT_DIR}/idle.elf" \
    "${OUTPUT_DIR}/crt0.o" \
    "${OUTPUT_DIR}/platform-exit.o" \
    "${OUTPUT_DIR}/idle-main.o"

for core_id in "${ACTIVE_CORE_IDS[@]}"; do
    elf="${OUTPUT_DIR}/core-${core_id}.elf"
    link_elf \
        "${elf}" \
        "${OUTPUT_DIR}/crt0.o" \
        "${OUTPUT_DIR}/uart.o" \
        "${OUTPUT_DIR}/platform-exit.o" \
        "${OUTPUT_DIR}/freestanding-memory.o" \
        "${OUTPUT_DIR}/freestanding-math.o" \
        "${OUTPUT_DIR}/mlir-runtime.o" \
        "${OUTPUT_DIR}/tile-main.o" \
        "${COMPILER_ARTIFACTS}/core-${core_id}.o" \
        "${RUNTIME_LIBRARY}"

    entry="$("${LLVM}/bin/llvm-readelf" --file-header "${elf}" |
        awk '/Entry point address:/ {print $4}')"
    if [[ "${entry}" != "0x80000000" ]]; then
        echo "core ${core_id} has unexpected entry point ${entry}" >&2
        exit 1
    fi
    if "${LLVM}/bin/llvm-nm" --undefined-only \
        --format=just-symbols "${elf}" | grep '.' >/dev/null; then
        echo "core ${core_id} has undefined symbols" >&2
        "${LLVM}/bin/llvm-nm" --undefined-only "${elf}" >&2
        exit 1
    fi
    echo "linked GPT-2 deployment core ${core_id}: ${elf}"
done

printf '%s\n' "${ACTIVE_CORE_IDS[@]}" >"${ACTIVE_CORE_MANIFEST}"
if [[ -f "${LOWERING_DIR}/deployment-routes.csv" ]]; then
    cp -- "${LOWERING_DIR}/deployment-routes.csv" "${ROUTE_MANIFEST}"
elif [[ -f "${LOWERING_DIR}/partitioned.mlir" ]]; then
    "${ROUTE_EXTRACTOR}" \
        --mesh-width "${MESH_WIDTH}" \
        "${LOWERING_DIR}/partitioned.mlir" \
        "${ROUTE_MANIFEST}"
else
    mapfile -t ISOLATED_MLIR_FILES < <(
        find "${COMPILER_ARTIFACTS}" \
            -maxdepth 1 \
            -type f \
            -name 'core-*-isolated.mlir' \
            -print |
            sort -V
    )
    if [[ "${#ISOLATED_MLIR_FILES[@]}" -gt 0 ]]; then
        "${ROUTE_EXTRACTOR}" \
            --mesh-width "${MESH_WIDTH}" \
            "${ISOLATED_MLIR_FILES[@]}" \
            "${ROUTE_MANIFEST}"
    else
        rm -f -- "${ROUTE_MANIFEST}"
    fi
fi
echo "linked ${#ACTIVE_CORE_IDS[@]} active GPT-2 tile ELFs and one idle ELF"
