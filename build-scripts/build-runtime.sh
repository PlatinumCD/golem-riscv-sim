#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly LLVM="${GOLEM_LLVM_DIR:-${INSTALL_ROOT}/llvm}"
readonly CLANGXX="${LLVM}/bin/clang++"
readonly LLVM_AR="${LLVM}/bin/llvm-ar"
readonly LLVM_NM="${LLVM}/bin/llvm-nm"
readonly OBJECT_DIR="${BUILD_ROOT}/runtime/objects"
readonly INSTALL_INCLUDE_DIR="${INSTALL_ROOT}/runtime/include/golem/runtime"
readonly INSTALL_LIBRARY_DIR="${INSTALL_ROOT}/runtime/lib"
readonly LIBRARY="${INSTALL_LIBRARY_DIR}/libgolem-runtime.a"

for executable in "${CLANGXX}" "${LLVM_AR}" "${LLVM_NM}"; do
    require_executable "${executable}"
done

sources=(
    ready_queue.cpp
    task_instance.cpp
    task_registry.cpp
    transport.cpp
)

cxx_flags=(
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
    "-I${PROJECT_ROOT}/runtime/include"
)

mkdir -p -- \
    "${OBJECT_DIR}" \
    "${INSTALL_INCLUDE_DIR}" \
    "${INSTALL_LIBRARY_DIR}"

objects=()
for source in "${sources[@]}"; do
    object="${OBJECT_DIR}/${source%.cpp}.o"
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "${PROJECT_ROOT}/runtime/src/${source}" \
        -o "${object}"
    objects+=("${object}")
done

rm -f -- \
    "${LIBRARY}" \
    "${OBJECT_DIR}/packet.o" \
    "${INSTALL_INCLUDE_DIR}/packet.h"
"${LLVM_AR}" rcsD "${LIBRARY}" "${objects[@]}"

for header in "${PROJECT_ROOT}"/runtime/include/golem/runtime/*.h; do
    install -m 0644 -- "${header}" "${INSTALL_INCLUDE_DIR}/"
done

if ! "${LLVM_NM}" --defined-only "${LIBRARY}" | grep -q "TaskInstancePool"; then
    echo "runtime archive is missing TaskInstancePool symbols" >&2
    exit 1
fi

echo "installed ${LIBRARY}"
