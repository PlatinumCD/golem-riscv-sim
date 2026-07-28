#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/torch-mlir-sculptor"
readonly TORCH_MLIR_PYTHON="${INSTALL_ROOT}/torch-mlir/python_packages/torch_mlir"
readonly SCULPTOR_OPT="${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt"
readonly LINALG_MLIR="${OUTPUT_DIR}/two-linear-linalg.mlir"
readonly CANONICAL_MLIR="${OUTPUT_DIR}/two-linear-sculptor.mlir"
readonly EXTRACTED_MLIR="${OUTPUT_DIR}/two-linear-extracted.mlir"
readonly MVM_MLIR="${OUTPUT_DIR}/two-linear-mvm.mlir"
readonly GOLEM_MLIR="${OUTPUT_DIR}/two-linear-golem.mlir"
readonly TASKS_MLIR="${OUTPUT_DIR}/two-linear-tasks.mlir"
readonly TASK_GRAPH_MLIR="${OUTPUT_DIR}/two-linear-task-graph.mlir"
readonly ISLANDS_MLIR="${OUTPUT_DIR}/two-linear-islands.mlir"
readonly SCHEDULED_MLIR="${OUTPUT_DIR}/two-linear-scheduled.mlir"
readonly FUSED_MLIR="${OUTPUT_DIR}/two-linear-fused.mlir"
readonly LLVM_SHIMS_MLIR="${OUTPUT_DIR}/two-linear-llvm-shims.mlir"
readonly FINALIZED_MLIR="${OUTPUT_DIR}/two-linear-finalized.mlir"
readonly LLVM_MLIR="${OUTPUT_DIR}/two-linear-llvm.mlir"

require_executable "${COMPILER_PYTHON}"
require_executable "${SCULPTOR_OPT}"
require_file "${TORCH_MLIR_PYTHON}/torch_mlir/fx.py"
mkdir -p -- "${OUTPUT_DIR}"

PYTHONPATH="${TORCH_MLIR_PYTHON}" \
    "${COMPILER_PYTHON}" \
    "${TEST_DIR}/import-model.py" \
    "${LINALG_MLIR}"

if [[ "$(grep -c 'linalg.matmul' "${LINALG_MLIR}")" -ne 2 ]]; then
    echo "Torch-MLIR output does not contain two linear matmuls" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${LINALG_MLIR}" \
    --sculptor-canonicalize-layers \
    -o "${CANONICAL_MLIR}"

if [[ "$(grep -c 'sculptor.nn.linear' "${CANONICAL_MLIR}")" -ne 2 ]]; then
    echo "Sculptor did not canonicalize both linear layers" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${CANONICAL_MLIR}" \
    --sculptor-extract-layers \
    -o "${EXTRACTED_MLIR}"

if [[ "$(grep -c 'sculptor.nn.linear' "${EXTRACTED_MLIR}")" -ne 2 ]] ||
   [[ "$(grep -c 'layer_type = \"linear_w_bias\"' "${EXTRACTED_MLIR}")" -ne 2 ]]; then
    echo "Sculptor did not extract two biased linear layer functions" >&2
    exit 1
fi
if grep -q 'linalg.matmul' "${EXTRACTED_MLIR}"; then
    echo "extracted Sculptor IR unexpectedly retains a Linalg matmul" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${EXTRACTED_MLIR}" \
    --sculptor-convert-layers \
    -o "${MVM_MLIR}"

if [[ "$(grep -c ' = sculptor.mvm ' "${MVM_MLIR}")" -ne 2 ]]; then
    echo "Sculptor did not convert both linear layers to MVM operations" >&2
    exit 1
fi
if grep -q 'sculptor.nn.linear' "${MVM_MLIR}"; then
    echo "converted Sculptor IR unexpectedly retains a linear operation" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${MVM_MLIR}" \
    --sculptor-expand-mvm-to-golem="array-rows=8 array-cols=8" \
    -o "${GOLEM_MLIR}"

for operation in set load execute store; do
    if [[ "$(grep -c "sculptor.array.${operation}" "${GOLEM_MLIR}")" -ne 2 ]]; then
        echo "expanded Sculptor IR does not contain two array.${operation} operations" >&2
        exit 1
    fi
done
if [[ "$(grep -c 'kind = \"sculptor.mvm\"' "${GOLEM_MLIR}")" -ne 2 ]] ||
   ! grep -Fq 'sculptor.tile_physical_shape = [8, 8]' "${GOLEM_MLIR}" ||
   ! grep -Fq 'sculptor.tile_valid_shape = [3, 4]' "${GOLEM_MLIR}" ||
   ! grep -Fq 'sculptor.tile_valid_shape = [2, 3]' "${GOLEM_MLIR}"; then
    echo "expanded Sculptor IR has an unexpected 8x8 array layout" >&2
    exit 1
fi
if grep -q ' = sculptor.mvm ' "${GOLEM_MLIR}"; then
    echo "expanded Sculptor IR unexpectedly retains an MVM operation" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${GOLEM_MLIR}" \
    --sculptor-materialize-tasks \
    -o "${TASKS_MLIR}"

if [[ "$(grep -c 'func.func private @task_' "${TASKS_MLIR}")" -ne 10 ]] ||
   [[ "$(grep -c 'call @task_' "${TASKS_MLIR}")" -ne 10 ]]; then
    echo "Sculptor did not materialize the expected ten callable tasks" >&2
    exit 1
fi
if grep -q 'sculptor.task_region' "${TASKS_MLIR}"; then
    echo "materialized Sculptor IR unexpectedly retains an inline task region" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${TASKS_MLIR}" \
    --sculptor-assemble-task-graph \
    -o "${TASK_GRAPH_MLIR}"

if [[ "$(grep -c 'sculptor.task_graph.create' "${TASK_GRAPH_MLIR}")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.task.create' "${TASK_GRAPH_MLIR}")" -ne 10 ]] ||
   [[ "$(grep -c 'sculptor.task_graph.input' "${TASK_GRAPH_MLIR}")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.task_graph.output' "${TASK_GRAPH_MLIR}")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.task_graph.intermediate' "${TASK_GRAPH_MLIR}")" -ne 9 ]]; then
    echo "Sculptor assembled an unexpected two-layer task graph" >&2
    exit 1
fi
for task_kind in \
    sculptor.matrix_setup \
    digital.vector_tile \
    sculptor.mvm \
    digital.tile_recombine \
    digital.bias_add; do
    if [[ "$(grep 'sculptor.task.create' "${TASK_GRAPH_MLIR}" |
        grep -c "task_kind = \"${task_kind}\"")" -ne 2 ]]; then
        echo "task graph does not contain two ${task_kind} nodes" >&2
        exit 1
    fi
done

"${SCULPTOR_OPT}" "${TASK_GRAPH_MLIR}" \
    --sculptor-build-task-graph-islands \
    -o "${ISLANDS_MLIR}"

if [[ "$(grep -c 'sculptor.schedule.island_id' "${ISLANDS_MLIR}")" -ne 10 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_0\".*sculptor.schedule.island_id = 0 : i64' "${ISLANDS_MLIR}")" -ne 5 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_1\".*sculptor.schedule.island_id = 5 : i64' "${ISLANDS_MLIR}")" -ne 5 ]]; then
    echo "Sculptor did not build the expected two five-task placement islands" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${ISLANDS_MLIR}" \
    --sculptor-schedule-task-graph="cores=2 arrays-per-core=1 schedule=snake" \
    -o "${SCHEDULED_MLIR}"

if [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_0\".*sculptor.runtime.core_id = 0 : i64' "${SCHEDULED_MLIR}")" -ne 5 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_1\".*sculptor.runtime.core_id = 1 : i64' "${SCHEDULED_MLIR}")" -ne 5 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_0\".*sculptor.runtime.local_array_id = 0 : i64.*sculptor.runtime.physical_array_id = 0 : i64' "${SCHEDULED_MLIR}")" -ne 2 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_1\".*sculptor.runtime.local_array_id = 0 : i64.*sculptor.runtime.physical_array_id = 1 : i64' "${SCHEDULED_MLIR}")" -ne 2 ]]; then
    echo "snake scheduling did not place the two islands on separate cores and arrays" >&2
    exit 1
fi
if ! grep -Fq 'sculptor.schedule.num_cores = 2 : i64' "${SCHEDULED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.arrays_per_core = 1 : i64' "${SCHEDULED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.logical_array_to_analog_array = [0, 1]' "${SCHEDULED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.inter_core_transfer_bytes = 12 : i64' "${SCHEDULED_MLIR}"; then
    echo "scheduled Sculptor IR has unexpected hardware or transfer metadata" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${SCHEDULED_MLIR}" \
    --sculptor-fuse-task-graph \
    -o "${FUSED_MLIR}"

if [[ "$(grep -c 'sculptor.task.create' "${FUSED_MLIR}")" -ne 4 ]] ||
   [[ "$(grep -c 'func.func private @task_' "${FUSED_MLIR}")" -ne 4 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*task_kind = \"sculptor.matrix_setup\"' "${FUSED_MLIR}")" -ne 2 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*task_kind = \"mixed.fused\"' "${FUSED_MLIR}")" -ne 2 ]]; then
    echo "Sculptor did not fuse the scheduled graph into two setup and two compute tasks" >&2
    exit 1
fi
if grep -q 'func.func @forward' "${FUSED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.task_count = 4 : i64' "${FUSED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.dependency_count = 3 : i64' "${FUSED_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.inter_core_transfer_bytes = 12 : i64' "${FUSED_MLIR}"; then
    echo "fused Sculptor IR has unexpected graph metadata or retains forward" >&2
    exit 1
fi
if [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_0\".*sculptor.runtime.core_id = 0 : i64' "${FUSED_MLIR}")" -ne 2 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_1\".*sculptor.runtime.core_id = 1 : i64' "${FUSED_MLIR}")" -ne 2 ]]; then
    echo "task fusion did not preserve the scheduled core ownership" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${FUSED_MLIR}" \
    --sculptor-lower-golem-to-llvm-shims \
    -o "${LLVM_SHIMS_MLIR}"

for shim in set load compute store; do
    if [[ "$(grep -c "func.func private @golem_analog_mvm_${shim}" "${LLVM_SHIMS_MLIR}")" -ne 1 ]] ||
       [[ "$(grep -c "call @golem_analog_mvm_${shim}" "${LLVM_SHIMS_MLIR}")" -ne 2 ]]; then
        echo "Golem lowering did not produce the expected ${shim} shim and calls" >&2
        exit 1
    fi
done
if grep -Eq 'sculptor\.array\.(set|load|execute|store)' "${LLVM_SHIMS_MLIR}"; then
    echo "LLVM-shim IR unexpectedly retains a Golem array operation" >&2
    exit 1
fi
if [[ "$(grep -c 'sculptor.task.create' "${LLVM_SHIMS_MLIR}")" -ne 4 ]] ||
   [[ "$(grep -c 'sculptor.task_graph.intermediate' "${LLVM_SHIMS_MLIR}")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*task_kind = \"sculptor.matrix_setup\".*inputs\[\], outputs\[\]' "${LLVM_SHIMS_MLIR}")" -ne 2 ]] ||
   ! grep -Fq 'sculptor.schedule.num_cores = 2 : i64' "${LLVM_SHIMS_MLIR}" ||
   ! grep -Fq 'sculptor.schedule.logical_array_to_analog_array = [0, 1]' "${LLVM_SHIMS_MLIR}"; then
    echo "LLVM-shim lowering produced an unexpected task-graph ABI" >&2
    exit 1
fi
if grep -q 'task_resource<!sculptor.logical.array>' "${LLVM_SHIMS_MLIR}"; then
    echo "LLVM-shim lowering unexpectedly retains a logical-array resource" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${LLVM_SHIMS_MLIR}" \
    --sculptor-finalize-task-graph-resources \
    -o "${FINALIZED_MLIR}"

if ! grep -Fq 'sculptor.runtime.input_slots = [0]' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.output_slots = [1]' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.resource_count = 3 : i64' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.temp_base_slot = 2 : i64' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.temp_count = 1 : i64' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.temp_offsets = [0]' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.workspace_size = 12 : i64' "${FINALIZED_MLIR}"; then
    echo "resource finalization produced unexpected graph-level runtime metadata" >&2
    exit 1
fi
for slot in 0 1 2; do
    if [[ "$(grep -c "sculptor.runtime.slot = ${slot} : i64" "${FINALIZED_MLIR}")" -ne 1 ]]; then
        echo "resource finalization did not assign runtime slot ${slot} exactly once" >&2
        exit 1
    fi
done
for task_index in 0 1 2 3; do
    if [[ "$(grep -c "sculptor.runtime.task_index = ${task_index} : i64" "${FINALIZED_MLIR}")" -ne 1 ]]; then
        echo "resource finalization did not assign task index ${task_index} exactly once" >&2
        exit 1
    fi
done
if ! grep -Fq 'sculptor.runtime.byte_size = 16 : i64, sculptor.runtime.slot = 0 : i64' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.byte_size = 8 : i64, sculptor.runtime.slot = 1 : i64' "${FINALIZED_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.byte_size = 12 : i64, sculptor.runtime.slot = 2 : i64' "${FINALIZED_MLIR}" ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_0\".*sculptor.runtime.input_slots = \[0\].*sculptor.runtime.output_slots = \[2\]' "${FINALIZED_MLIR}")" -ne 1 ]] ||
   [[ "$(grep -c 'sculptor.task.create.*source_layer = \"linearwbias_1\".*sculptor.runtime.input_slots = \[2\].*sculptor.runtime.output_slots = \[1\]' "${FINALIZED_MLIR}")" -ne 1 ]]; then
    echo "resource finalization produced unexpected tensor slots or task bindings" >&2
    exit 1
fi
if grep -q 'task_resource<!sculptor.logical.array>' "${FINALIZED_MLIR}"; then
    echo "resource finalization unexpectedly retains a logical-array resource" >&2
    exit 1
fi

"${SCULPTOR_OPT}" "${FINALIZED_MLIR}" \
    --canonicalize \
    --cse \
    --empty-tensor-to-alloc-tensor \
    --one-shot-bufferize="bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map" \
    --convert-bufferization-to-memref \
    --convert-linalg-to-loops \
    --lower-affine \
    --convert-scf-to-cf \
    --convert-math-to-llvm \
    --expand-strided-metadata \
    --lower-affine \
    --convert-arith-to-llvm \
    --convert-index-to-llvm \
    --convert-cf-to-llvm \
    --finalize-memref-to-llvm \
    --convert-func-to-llvm \
    --reconcile-unrealized-casts \
    -o "${LLVM_MLIR}"

for dialect in linalg tensor bufferization scf affine arith math memref cf; do
    if grep -Eq "(^|[[:space:]])${dialect}\\.[[:alnum:]_]+" "${LLVM_MLIR}"; then
        echo "LLVM lowering unexpectedly retains a ${dialect} operation" >&2
        exit 1
    fi
done
if grep -q 'unrealized_conversion_cast' "${LLVM_MLIR}" ||
   [[ "$(grep -c 'llvm.func @task_' "${LLVM_MLIR}")" -ne 4 ]] ||
   [[ "$(grep -c 'llvm.mlir.global private constant' "${LLVM_MLIR}")" -ne 5 ]]; then
    echo "LLVM lowering did not fully convert the four task implementations" >&2
    exit 1
fi
for shim in set load compute store; do
    if [[ "$(grep -c "llvm.func @golem_analog_mvm_${shim}" "${LLVM_MLIR}")" -ne 1 ]] ||
       [[ "$(grep -c "llvm.call @golem_analog_mvm_${shim}" "${LLVM_MLIR}")" -ne 2 ]]; then
        echo "LLVM lowering did not preserve the expected ${shim} shim interface" >&2
        exit 1
    fi
done
if [[ "$(grep -c 'sculptor.task.create' "${LLVM_MLIR}")" -ne 4 ]] ||
   [[ "$(grep -c 'sculptor.runtime.task_index' "${LLVM_MLIR}")" -ne 4 ]] ||
   ! grep -Fq 'func.func private @generate_task_graph() -> !sculptor.task_graph' "${LLVM_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.resource_count = 3 : i64' "${LLVM_MLIR}" ||
   ! grep -Fq 'sculptor.runtime.workspace_size = 12 : i64' "${LLVM_MLIR}"; then
    echo "LLVM lowering did not preserve the finalized task-graph metadata" >&2
    exit 1
fi
if grep -q 'task_resource<!sculptor.logical.array>' "${LLVM_MLIR}"; then
    echo "LLVM lowering unexpectedly reintroduced a logical-array resource" >&2
    exit 1
fi

cat "${EXTRACTED_MLIR}"
echo "Expanded 8x8 Golem IR: ${GOLEM_MLIR}"
echo "Materialized task IR: ${TASKS_MLIR}"
echo "Assembled task graph IR: ${TASK_GRAPH_MLIR}"
echo "Logical placement islands IR: ${ISLANDS_MLIR}"
echo "Two-core snake-scheduled IR: ${SCHEDULED_MLIR}"
echo "Same-core fused task graph IR: ${FUSED_MLIR}"
echo "Golem shim and task-graph ABI IR: ${LLVM_SHIMS_MLIR}"
echo "Finalized tensor resource IR: ${FINALIZED_MLIR}"
echo "LLVM-dialect task implementation IR: ${LLVM_MLIR}"
echo "PyTorch -> Torch-MLIR -> Sculptor task graph -> schedule -> resources -> LLVM task code: PASS"
