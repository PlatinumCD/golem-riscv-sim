#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly ACTION="${1:-all}"
readonly LLVM="${GOLEM_LLVM_DIR:-${INSTALL_ROOT}/llvm}"
readonly CLANG="${LLVM}/bin/clang"
readonly CLANGXX="${LLVM}/bin/clang++"
readonly READELF="${LLVM}/bin/llvm-readelf"

for executable in "${CLANG}" "${CLANGXX}" "${READELF}"; do
    require_executable "${executable}"
done

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
    -g
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
    "-I${PROJECT_ROOT}/platform"
)

build_tile() {
    local output_dir="$1"
    local image_name="$2"
    local application_source="$3"
    local application_object="${output_dir}/${image_name}.o"
    local elf="${output_dir}/${image_name}.elf"
    local entry

    mkdir -p -- "${output_dir}"

    "${CLANG}" "${common_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/crt0.S" \
        -o "${output_dir}/crt0.o"
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/uart.cpp" \
        -o "${output_dir}/uart.o"
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/platform/platform-exit.cpp" \
        -o "${output_dir}/platform-exit.o"
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "${application_source}" \
        -o "${application_object}"

    "${CLANGXX}" "${common_flags[@]}" \
        -nostdlib \
        -nostartfiles \
        -nodefaultlibs \
        -fuse-ld=lld \
        -Wl,--build-id=none \
        -Wl,--gc-sections \
        "-Wl,-T,${PROJECT_ROOT}/platform/tile.ld" \
        "-Wl,-Map,${output_dir}/${image_name}.map" \
        "${output_dir}/crt0.o" \
        "${output_dir}/uart.o" \
        "${output_dir}/platform-exit.o" \
        "${application_object}" \
        -o "${elf}"

    entry="$(${READELF} --file-header "${elf}" | \
        awk '/Entry point address:/ { print $4 }')"
    if [[ "${entry}" != "0x80000000" ]]; then
        echo "unexpected entry point for ${image_name}: ${entry}" >&2
        return 1
    fi
    if "${READELF}" --program-headers "${elf}" | grep -q INTERP; then
        echo "${image_name} unexpectedly contains a dynamic interpreter" >&2
        return 1
    fi

    echo "built ${elf}"
}

build_hello() {
    build_tile \
        "${BUILD_ROOT}/tests/hello" \
        hello \
        "${PROJECT_ROOT}/tests/hello/main.cpp"
}

build_mesh_pair() {
    local output_dir="${BUILD_ROOT}/tests/mesh-pair"
    build_tile \
        "${output_dir}" \
        tile0-sender \
        "${PROJECT_ROOT}/tests/mesh-pair/sender.cpp"
    build_tile \
        "${output_dir}" \
        tile1-receiver \
        "${PROJECT_ROOT}/tests/mesh-pair/receiver.cpp"
}

case "${ACTION}" in
    all)
        build_hello
        build_mesh_pair
        ;;
    hello) build_hello ;;
    mesh-pair) build_mesh_pair ;;
    *)
        echo "usage: $0 [all|hello|mesh-pair]" >&2
        exit 2
        ;;
esac
