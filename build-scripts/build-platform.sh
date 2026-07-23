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
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"

for executable in "${CLANG}" "${CLANGXX}" "${READELF}"; do
    require_executable "${executable}"
done

"${SCRIPT_DIR}/build-runtime.sh"
require_file "${RUNTIME_LIBRARY}"

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
    "-I${RUNTIME_INCLUDE}"
    "-I${PROJECT_ROOT}/platform"
)

build_tile() {
    local output_dir="$1"
    local image_name="$2"
    local application_source="$3"
    shift 3
    local application_flags=("$@")
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
        "${application_flags[@]}" \
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
        "${RUNTIME_LIBRARY}" \
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

build_runtime_library() {
    build_tile \
        "${BUILD_ROOT}/tests/runtime-library" \
        runtime-library \
        "${PROJECT_ROOT}/tests/runtime-library/main.cpp"
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

build_mesh_3x3() {
    local output_dir="${BUILD_ROOT}/tests/mesh-3x3"
    local tile_id

    build_tile \
        "${output_dir}" \
        tile0-sender \
        "${PROJECT_ROOT}/tests/mesh-3x3/sender.cpp"

    for tile_id in {1..7}; do
        build_tile \
            "${output_dir}" \
            "tile${tile_id}-worker" \
            "${PROJECT_ROOT}/tests/mesh-3x3/worker.cpp" \
            "-DMITTENS_TILE_ID=${tile_id}"
    done

    build_tile \
        "${output_dir}" \
        tile8-receiver \
        "${PROJECT_ROOT}/tests/mesh-3x3/receiver.cpp"
}

build_mesh_pipeline() {
    local output_dir="${BUILD_ROOT}/tests/mesh-pipeline"
    local tile_id
    local -a successors expected_inputs expected_outputs

    successors[1]=2
    successors[2]=5
    successors[3]=6
    successors[4]=3
    successors[5]=4
    successors[6]=7
    successors[7]=8
    successors[8]=0

    expected_inputs[1]=0x00000000
    expected_inputs[2]=0x3f800000
    expected_inputs[3]=0x41400000
    expected_inputs[4]=0x41000000
    expected_inputs[5]=0x40400000
    expected_inputs[6]=0x41700000
    expected_inputs[7]=0x41a80000
    expected_inputs[8]=0x41e00000

    expected_outputs[1]=0x3f800000
    expected_outputs[2]=0x40400000
    expected_outputs[3]=0x41700000
    expected_outputs[4]=0x41400000
    expected_outputs[5]=0x41000000
    expected_outputs[6]=0x41a80000
    expected_outputs[7]=0x41e00000
    expected_outputs[8]=0x42100000

    build_tile \
        "${output_dir}" \
        tile0-dispatcher \
        "${PROJECT_ROOT}/tests/mesh-pipeline/dispatcher.cpp"

    for tile_id in {1..8}; do
        build_tile \
            "${output_dir}" \
            "tile${tile_id}-worker" \
            "${PROJECT_ROOT}/tests/mesh-pipeline/worker.cpp" \
            "-DMITTENS_TILE_ID=${tile_id}" \
            "-DMITTENS_NEXT_TILE=${successors[${tile_id}]}" \
            "-DMITTENS_EXPECTED_INPUT=${expected_inputs[${tile_id}]}U" \
            "-DMITTENS_EXPECTED_OUTPUT=${expected_outputs[${tile_id}]}U"
    done
}

build_distributed_matvec_variant() {
    local output_dir="$1"
    shift
    local mapping_flags=("$@")
    local tile_id

    for tile_id in {0..8}; do
        build_tile \
            "${output_dir}" \
            "tile${tile_id}" \
            "${PROJECT_ROOT}/tests/distributed-matvec/main.cpp" \
            "-DMITTENS_TILE_ID=${tile_id}" \
            "${mapping_flags[@]}"
    done
}

build_distributed_matvec() {
    build_distributed_matvec_variant \
        "${BUILD_ROOT}/tests/distributed-matvec"
}

build_distributed_matvec_reverse() {
    build_distributed_matvec_variant \
        "${BUILD_ROOT}/tests/distributed-matvec-reverse" \
        -DMITTENS_REVERSE_SECOND_PASS=1
}

case "${ACTION}" in
    all)
        build_hello
        build_runtime_library
        build_mesh_pair
        build_mesh_3x3
        build_mesh_pipeline
        build_distributed_matvec
        ;;
    hello) build_hello ;;
    runtime-library) build_runtime_library ;;
    mesh-pair) build_mesh_pair ;;
    mesh-3x3) build_mesh_3x3 ;;
    mesh-pipeline) build_mesh_pipeline ;;
    distributed-matvec) build_distributed_matvec ;;
    distributed-matvec-reverse) build_distributed_matvec_reverse ;;
    *)
        echo "usage: $0 [all|hello|runtime-library|mesh-pair|mesh-3x3|mesh-pipeline|distributed-matvec|distributed-matvec-reverse]" >&2
        exit 2
        ;;
esac
