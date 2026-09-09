#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
readonly OUTPUT="${TEST_RESULTS_ROOT}/materialized-functional-infrastructure"
readonly RUNTIME_SOURCE="${PROJECT_ROOT}/third_party/sculptor-mlir/runtime"
readonly CXX="${CXX:-c++}"

mkdir -p -- "${OUTPUT}"
"${CXX}" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
    -I"${PROJECT_ROOT}" -I"${RUNTIME_SOURCE}/include" \
    -I"${RUNTIME_SOURCE}/src" \
    -I"${TEST_DIR}" \
    "${TEST_DIR}/abi_accounting.cpp" "${TEST_DIR}/abi_accounting_test.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_shard_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_parametric_routes.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_execution_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_physical.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_aliases.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_owner_alias_coverage.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_descriptor.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_descriptors.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_output_ownership.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_exhaustive_families.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_port_coverage.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_admission_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_digest.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_binding_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_direct_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_index.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_memory_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_periodic_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_physical_families.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_resource_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_route_math.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_sequence_overlap.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_shard_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_accessors.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_periodic_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_materialized_resolution.cpp" \
    "${RUNTIME_SOURCE}/src/task_registry.cpp" \
    "${RUNTIME_SOURCE}/src/materialized_dma_ownership.cpp" \
    -o "${OUTPUT}/abi-accounting-test"
"${OUTPUT}/abi-accounting-test"

"${CXX}" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
    -I"${PROJECT_ROOT}" -I"${RUNTIME_SOURCE}/include" \
    -I"${RUNTIME_SOURCE}/src" \
    "${TEST_DIR}/model_input_seed_test.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_shard_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_parametric_routes.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_execution_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_physical.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_aliases.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_owner_alias_coverage.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_descriptor.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_descriptors.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_output_ownership.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_exhaustive_families.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_port_coverage.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_admission_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_digest.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_binding_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_direct_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_index.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_dma_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_memory_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_periodic_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_physical_families.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_residency_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_resource_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_route_math.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_sequence_overlap.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_shard_validation.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_accessors.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_affine_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_periodic_records.cpp" \
    "${RUNTIME_SOURCE}/src/tile_abi_materialized_resolution.cpp" \
    "${RUNTIME_SOURCE}/src/task_registry.cpp" \
    "${RUNTIME_SOURCE}/src/materialized_dma_ownership.cpp" \
    -o "${OUTPUT}/model-input-seed-test"
"${OUTPUT}/model-input-seed-test"

for case_id in 1 2 3 4 5 6; do
    "${CXX}" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
        -I"${TEST_DIR}" "-DMITTENS_MATERIALIZED_CASE=${case_id}" \
        "${TEST_DIR}/case_oracle.cpp" "${TEST_DIR}/oracle_test.cpp" \
        -o "${OUTPUT}/oracle-${case_id}-test"
    "${OUTPUT}/oracle-${case_id}-test"
done

python3 "${TEST_DIR}/infrastructure_test.py"
echo "materialized functional negative gates: PASS (ABI/version/structure/manifest/routes/4KiB/accounting/profile/output)"
