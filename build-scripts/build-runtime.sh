#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly LLVM="${GOLEM_LLVM_DIR:-${INSTALL_ROOT}/llvm}"
readonly CLANGXX="${LLVM}/bin/clang++"
readonly LLVM_AR="${LLVM}/bin/llvm-ar"
readonly LLVM_NM="${LLVM}/bin/llvm-nm"
readonly SCULPTOR_RUNTIME_SOURCE="${PROJECT_ROOT}/third_party/sculptor-mlir/runtime"
readonly OBJECT_DIR="${BUILD_ROOT}/runtime/objects"
readonly INSTALL_INCLUDE_DIR="${INSTALL_ROOT}/runtime/include/golem/runtime"
readonly INSTALL_ABI_INCLUDE_DIR="${INSTALL_ROOT}/runtime/include/golem/abi"
readonly INSTALL_LIBRARY_DIR="${INSTALL_ROOT}/runtime/lib"
readonly LIBRARY="${INSTALL_LIBRARY_DIR}/libgolem-runtime.a"
readonly CODEGEN_OPT_LEVEL="${GOLEM_CODEGEN_OPT_LEVEL:--O2}"
readonly CODEGEN_LTO="${GOLEM_CODEGEN_LTO:-none}"
readonly RUNTIME_PROFILE="${GOLEM_RUNTIME_ENABLE_PROFILE:-0}"

case "${CODEGEN_OPT_LEVEL}" in
    -O0|-O1|-O2|-O3|-Os|-Oz) ;;
    *)
        echo "unsupported GOLEM_CODEGEN_OPT_LEVEL: ${CODEGEN_OPT_LEVEL}" >&2
        exit 2
        ;;
esac
case "${CODEGEN_LTO}" in
    none|thin|full) ;;
    *)
        echo "GOLEM_CODEGEN_LTO must be none, thin, or full" >&2
        exit 2
        ;;
esac
case "${RUNTIME_PROFILE}" in
    0|1) ;;
    *)
        echo "GOLEM_RUNTIME_ENABLE_PROFILE must be 0 or 1" >&2
        exit 2
        ;;
esac

optimization_flags=("${CODEGEN_OPT_LEVEL}")
profile_flags=()
if [[ "${CODEGEN_LTO}" != "none" ]]; then
    optimization_flags+=("-flto=${CODEGEN_LTO}")
fi
if [[ "${RUNTIME_PROFILE}" == "1" ]]; then
    profile_flags+=("-DGOLEM_RUNTIME_ENABLE_PROFILE=1")
fi

for executable in "${CLANGXX}" "${LLVM_AR}" "${LLVM_NM}"; do
    require_executable "${executable}"
done
require_file "${SCULPTOR_RUNTIME_SOURCE}/src/deployment_runtime.cpp"
require_file "${SCULPTOR_RUNTIME_SOURCE}/src/heap_profile.cpp"
require_file "${SCULPTOR_RUNTIME_SOURCE}/src/materialized_dma_ownership.cpp"
require_file "${SCULPTOR_RUNTIME_SOURCE}/src/mlir_runtime.cpp"

sources=(
    basic_tile_runtime.cpp
    deployment_runtime.cpp
    deployment_lifecycle.cpp
    deployment_tasks.cpp
    deployment_shards.cpp
    deployment_residency.cpp
    deployment_receive.cpp
    deployment_transmit.cpp
    deployment_dma.cpp
    deployment_completion.cpp
    deployment_diagnostics.cpp
    heap_profile.cpp
    materialized_dma_ownership.cpp
    ready_queue.cpp
    routed_transport.cpp
    scratchpad_abi.cpp
    task_instance.cpp
    task_registry.cpp
    tile_abi.cpp
    tile_abi_shard_records.cpp
    tile_abi_parametric_routes.cpp
    tile_abi_execution_residency_validation.cpp
    tile_abi_residency_physical.cpp
    tile_abi_residency_aliases.cpp
    tile_abi_owner_alias_coverage.cpp
    tile_abi_dma_descriptor.cpp
    tile_abi_dma_descriptors.cpp
    tile_abi_dma_output_ownership.cpp
    tile_abi_dma_exhaustive_families.cpp
    tile_abi_dma_port_coverage.cpp
    tile_abi_admission_validation.cpp
    tile_abi_affine_digest.cpp
    tile_abi_affine_validation.cpp
    tile_abi_binding_validation.cpp
    tile_abi_direct_residency_validation.cpp
    tile_abi_dma_index.cpp
    tile_abi_dma_validation.cpp
    tile_abi_memory_validation.cpp
    tile_abi_periodic_validation.cpp
    tile_abi_physical_families.cpp
    tile_abi_residency_validation.cpp
    tile_abi_resource_validation.cpp
    tile_abi_route_math.cpp
    tile_abi_sequence_overlap.cpp
    tile_abi_shard_validation.cpp
    tile_abi_accessors.cpp
    tile_abi_affine_records.cpp
    tile_abi_periodic_records.cpp
    tile_abi_materialized_resolution.cpp
    transport.cpp
    mlir_runtime.cpp
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
    "${optimization_flags[@]}"
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
    "-I${SCULPTOR_RUNTIME_SOURCE}/include"
    "-I${SCULPTOR_RUNTIME_SOURCE}/src"
    "${profile_flags[@]}"
)

mkdir -p -- \
    "${OBJECT_DIR}" \
    "${INSTALL_INCLUDE_DIR}" \
    "${INSTALL_ABI_INCLUDE_DIR}" \
    "${INSTALL_LIBRARY_DIR}"

objects=()
for source in "${sources[@]}"; do
    object="${OBJECT_DIR}/${source%.cpp}.o"
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "${SCULPTOR_RUNTIME_SOURCE}/src/${source}" \
        -o "${object}"
    objects+=("${object}")
done

rm -f -- \
    "${LIBRARY}" \
    "${OBJECT_DIR}/packet.o" \
    "${INSTALL_INCLUDE_DIR}/packet.h"
"${LLVM_AR}" rcsD "${LIBRARY}" "${objects[@]}"

for header in "${SCULPTOR_RUNTIME_SOURCE}"/include/golem/runtime/*.h; do
    install -m 0644 -- "${header}" "${INSTALL_INCLUDE_DIR}/"
done

for header in "${SCULPTOR_RUNTIME_SOURCE}"/include/golem/abi/*.h; do
    install -m 0644 -- "${header}" "${INSTALL_ABI_INCLUDE_DIR}/"
done

if ! "${LLVM_NM}" --defined-only "${LIBRARY}" |
    grep "TaskInstancePool" >/dev/null; then
    echo "runtime archive is missing TaskInstancePool symbols" >&2
    exit 1
fi

echo "installed ${LIBRARY}"
