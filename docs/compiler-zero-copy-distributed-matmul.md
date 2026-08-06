# Compiler Instructions for Zero-Copy Distributed Matmul

## 1. Purpose

This document gives the required compiler changes for distributed digital
matmul.

The compiler must not copy an input tensor into one temporary buffer for each
shard.

The compiler must describe each shard with a tensor view.

The NIC must send the selected tensor data from the original storage.

This document uses ASD-STE100 Simplified Technical English.

## 2. Required result

The new pass behavior must produce this data flow:

```text
Base tensor
    |
    +-- view 0 --> shard 0
    +-- view 1 --> shard 1
    +-- view 2 --> shard 2
    +-- view 3 --> shard 3
```

The new pass behavior must not produce this data flow:

```text
Base tensor
    |
    v
CPU partition task
    |
    +-- copied buffer 0 --> shard 0
    +-- copied buffer 1 --> shard 1
    +-- copied buffer 2 --> shard 2
    +-- copied buffer 3 --> shard 3
```

The compiler must remove all executable `digital.matmul_partition` tasks from
the view mode.

The compiler must keep `digital.matmul_shard` tasks.

The compiler must keep `digital.matmul_assembly` tasks in the first version.

## 3. Terms

Use the terms in this section with the specified meanings.

**Base tensor** means the original tensor storage before distribution.

**Slice** means one selected region of a base tensor.

**Tensor view** means a descriptor for a slice. A tensor view does not own
tensor data.

**Materialize** means to copy tensor elements into new storage.

**Local view** means a view that a task uses on the producer tile.

**Remote view** means a view whose data must go to a different tile.

**Contiguous slice** means that all slice elements occupy one continuous byte
range.

**Shard task** means one digital matmul task that computes part of a result.

**Route buffer** means the destination storage for one inter-tile transfer.

## 4. Current defect

The current implementation creates partition functions in this file:

```text
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphMatmulDistributor.cpp
```

`createPartitionFunction()` creates one `tensor.extract_slice` result for each
shard.

`rewriteMatmul()` and `rewriteAttentionMatmul()` create intermediate resources
for these results.

These functions also create `digital.matmul_partition` tasks.

Bufferization gives storage to the partition results.

The Golem task adapter then copies each function result into a task output.

The result copy occurs in this file:

```text
lib/Dialect/Sculptor/Conversion/golem/GolemTileEntryShims.cpp
```

The adapter emits `LLVM::MemcpyOp` for each returned tensor.

The four-token GPT-2 test measured the following costs:

| Item | Measured value |
|---|---:|
| Partition task time | 2.358372 ms |
| Partition instructions | 4,716,696 |
| Partition time on the critical chain | 2.293078 ms |
| Distributed runtime | 14.600740 ms |
| Runtime without distribution | 12.109068 ms |

The mesh is not the main cause of this result.

Distribution reduced directional word-hops by 15.26 percent.

The partition copies removed the benefit from the shorter routes.

## 5. Scope for the first version

The first version must support static `f32` tensors.

The first version must support contiguous slices only.

The first version must support the GPT-2 attention-head distribution path.

The first version must support a static batch size of one.

The first version must not add scatter-gather DMA.

The first version must not change the 32-bit mesh transfer width.

The first version must not share a pointer between tiles.

The first version must keep the current output assembly task.

The compiler must keep an undistributed matmul when a view is not safe.

If `require-change=true`, the compiler must report why no safe view exists.

## 6. Pass interface

Add this option to `sculptor-distribute-digital-matmul`:

```text
partition-mode=copy|view
```

Use `copy` as the temporary default during development.

Use `view` for the new tests.

Make `view` the default after all acceptance tests pass.

The complete test option must be:

```bash
--sculptor-distribute-digital-matmul="\
strategy=auto max-shards=8 min-ops-per-shard=1 \
placement-policy=prefer-distinct require-change=true partition-mode=view"
```

The composite pipeline must expose the same option.

Use this composite option name:

```text
digital-matmul-partition-mode
```

## 7. New IR contract

Add one graph operation for a tensor view.

Use this operation name:

```text
sculptor.task_graph.slice
```

The operation must have these values:

```text
input:  one base task resource
output: one slice task resource
```

The operation must have these attributes:

```text
offsets: static element offsets for all dimensions
sizes:   static element counts for all dimensions
strides: static element strides for all dimensions
```

Use this form:

```mlir
%slice = sculptor.task_graph.slice %graph, %base {
  offsets = [4, 0, 0],
  sizes = [2, 4, 64],
  strides = [1, 1, 1]
} : (!sculptor.task_graph,
     !sculptor.task_resource<tensor<12x4x64xf32>>)
 -> !sculptor.task_resource<tensor<2x4x64xf32>>
```

The operation must not create a task token.

The operation must not own storage.

The operation must not create a scheduling island.

The operation must keep a reference to the base resource.

The verifier must apply these checks:

1. The input and output ranks must be equal.
2. All shapes must be static.
3. All element types must be equal.
4. All offsets must be zero or positive.
5. All sizes must be zero or positive.
6. All strides must equal one in the first version.
7. Each slice range must stay inside the base tensor.
8. The slice must describe one contiguous byte range.
9. All size and offset calculations must detect overflow.

The verifier must report the failed check and the operand number.

## 8. Distribution rewrite

Change `TaskGraphMatmulDistributor.cpp` as follows.

### 8.1 Remove partition tasks in view mode

Do not call `createPartitionFunction()` in view mode.

Do not create a `digital.matmul_partition` task in view mode.

Do not create a materialized intermediate resource for an operand slice.

Create one `sculptor.task_graph.slice` operation for each required operand
slice.

Give each shard task the applicable slice resource.

Keep the original producer task as the data dependency.

Keep all control-only dependencies from the original task.

Do not add a dependency on a view operation.

### 8.2 Keep shard computation unchanged

Keep the current shard count and shard IDs.

Keep the current `TaskDistributionAttr` values.

Keep the current digital operation count for each shard.

Keep the current placement policy.

Keep the current deterministic shard order.

### 8.3 Keep assembly in the first version

Keep the current `digital.matmul_assembly` task.

Keep the current output order.

Keep the current result shape.

Do not combine the assembly change with the first view change.

## 9. Contiguous layout rules

The compiler must prove contiguity before it creates a view.

Use the physical row-major layout for the proof.

Calculate the byte offset with this expression:

```text
byte_offset = element_offset * element_byte_size
```

Calculate `element_offset` from the base tensor strides and slice offsets.

Calculate the transfer size with this expression:

```text
byte_size = product(slice_sizes) * element_byte_size
```

Reject a view if the selected elements do not form one byte range.

Do not convert a non-contiguous slice into an implicit copy.

Do not hide a packing loop inside a shard function.

Do not charge a view as zero-cost if the view requires packing.

## 10. GPT-2 attention layout

The current GPT-2 query, key, and value layout makes head slices
non-contiguous.

Change the QKV split output layout for the view path.

For the current batch-one test, use this physical layout:

```text
[head, sequence, head_dimension]
```

Write each head directly to its final head-major location.

Do not first write `[sequence, hidden]` and then transpose the tensor.

Use these view shapes for one head group:

```text
query:       [head_count, query_length, head_dimension]
key:         [head_count, key_length,   head_dimension]
value:       [head_count, key_length,   head_dimension]
probability: [head_count, query_length, key_length]
```

The attention score shard must consume query and key views.

The attention apply shard must consume probability and value views.

The compiler must preserve the causal-mask behavior.

The compiler must preserve the head order.

The compiler must preserve the final logical output shape.

The compiler must reject view mode for a static batch size greater than one.

The compiler can add a general batch layout in a later change.

## 11. Placement and timing rules

Treat a slice resource as a data edge from the base producer.

Use the slice byte size for inter-core transfer cost.

Do not use the full base tensor size for slice transfer cost.

Use zero transfer cost when the producer and shard use the same tile.

Do not create a latency record for the view operation.

Do not add digital operations for the view operation.

Do not add CPU instructions for the view operation.

Keep the DMA and network costs for a remote view.

The scheduler must include the following remote-view costs:

```text
source DMA read
NIC protocol words
payload words
Manhattan link traversal
destination DMA write
```

The scheduler must not include a CPU partition-copy cost for a contiguous
view.

## 12. Per-core partition rules

The deployment partitioner must distinguish local views and remote views.

### 12.1 Local view

Lower a local view to `memref.subview`.

Pass the subview descriptor to the local shard function.

Do not allocate workspace storage for a local view.

Do not copy data for a local view.

Do not create a route for a local view.

### 12.2 Remote view

Create one route for each remote consumer.

Set the source task to the base tensor producer.

Set the source output to the base tensor output index.

Set the destination task to the shard task.

Set the destination input to the shard operand index.

Set the route byte size to the slice byte size.

Allocate one slice-sized route buffer on the destination tile.

Do not allocate a partition buffer on the source tile.

The destination shard must consume the destination route buffer.

## 13. Deployment route extension

Add one field to `#sculptor.deployment_route`:

```text
sourceByteOffset
```

The field must use bytes.

The field must be a static unsigned integer.

The field must be zero for a full-tensor route.

The field must identify the first byte of a slice in the base tensor.

Use this route form:

```mlir
#sculptor.deployment_route<
  id = 17,
  sourceCore = 2,
  sourceTask = 63,
  sourceOutput = 0,
  destinationCore = 25,
  destinationTask = 64,
  destinationInput = 0,
  resourceId = 81,
  byteSize = 2048,
  sourceByteOffset = 4096
>
```

The partitioner must verify this expression:

```text
sourceByteOffset + byteSize <= baseResourceByteSize
```

The partitioner must detect integer overflow in this expression.

The compiler must preserve one route ID for each transfer occurrence.

The compiler must preserve one global resource ID for each logical slice.

The compiler must also preserve the base resource ID on the slice operation.

Use this attribute name after global ID assignment:

```text
sculptor.deployment.base_global_resource_id
```

Before global ID assignment, the slice input must identify the base resource.

## 14. Tile ABI output

Extend the emitted route table with `source_byte_offset`.

Use this route field order:

```text
id
source_core
source_task
source_output
destination_core
destination_task
destination_input
global_resource_id
local_slot
byte_size
source_byte_offset
```

For an outgoing route, `local_slot` must identify the base tensor slot.

For an incoming route, `local_slot` must identify the route buffer slot.

The compiler must emit the same `source_byte_offset` on both route records.

The runtime must ignore `source_byte_offset` on the destination record.

The runtime must apply `source_byte_offset` before it sends the first payload
word.

This route-table change requires a matching runtime ABI change.

Do not change the packet format.

Do not send a pointer in a packet.

Do not change the payload word count.

The network must continue to send one 32-bit word per transfer operation.

## 15. Memory-model behavior

The native memory backend must read the slice from the base tensor address.

The native memory backend must not account for a partition copy.

The destination must still store every received payload word.

The MemHierarchy backend must model source DMA reads in a later integration.

The MemHierarchy backend must also model destination DMA writes.

The compiler must keep the route byte count independent of the memory backend.

The compiler must not assume shared memory between tiles.

## 16. Required file changes

Review and change these files:

```text
include/sculptor-mlir/Dialect/Sculptor/IR/Ops/SculptorTaskGraphOps.td
include/sculptor-mlir/Dialect/Sculptor/IR/SculptorAttrs.td
include/sculptor-mlir/Dialect/Sculptor/Transforms/DistributeDigitalMatmul.h
lib/Dialect/Sculptor/IR/Ops/SculptorTaskGraphOps.cpp
lib/Dialect/Sculptor/Transforms/DistributeDigitalMatmul.cpp
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphMatmulDistributor.cpp
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphDAG.cpp
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphResources.cpp
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphDeploymentPartitioner.cpp
lib/Dialect/Sculptor/Transforms/task_graph/TaskGraphExecutionGraph.cpp
lib/Dialect/Sculptor/Transforms/task_schedulers/TaskGraphScorer.cpp
lib/Dialect/Sculptor/Transforms/task_timing/TaskCostAnalysis.cpp
lib/Dialect/Sculptor/Transforms/assemblers/TaskGraphExecutionPlanAssembler.cpp
lib/Dialect/Sculptor/Conversion/golem/GolemTileABI.cpp
lib/Dialect/Sculptor/Conversion/golem/GolemTileABI.h
lib/Dialect/Sculptor/Conversion/golem/GolemTileTables.cpp
```

Change other files only when a verifier or an exporter consumes task
resources.

Update the graph visualization exporter.

Update the simulation-model exporter.

Update the route manifest exporter.

## 17. Required unit tests

Add one test for each item in this section.

1. A contiguous local slice becomes a view.
2. A contiguous remote slice becomes a slice route.
3. View mode creates no `digital.matmul_partition` task.
4. Copy mode keeps the current partition task.
5. A non-contiguous slice stays undistributed.
6. `require-change=true` reports a non-contiguous slice.
7. An out-of-range slice produces an error.
8. An offset calculation overflow produces an error.
9. A byte-size calculation overflow produces an error.
10. A local view has no workspace allocation.
11. A remote view allocates only the destination route buffer.
12. Route cost uses the slice byte size.
13. Route metadata contains the correct source byte offset.
14. Per-core extraction preserves the slice route.
15. Tile ABI emission preserves the source byte offset.
16. GPT-2 attention preserves head order.
17. GPT-2 attention preserves the causal mask.
18. Static batch size greater than one stays undistributed.

Run every test with `--verify-each`.

Do not use `--allow-unregistered-dialect` in an acceptance test.

## 18. Required integration tests

Compile the GPT-2 four-token model with view mode.

Use the same 12 by 12 experiment configuration.

Use four arrays per tile.

Use dual issue at 1 GHz.

Use a 100-cycle analog MVM latency.

Use reduction width two.

Use the current greedy-timing L3 and beam-eight schedule.

Use diagonal scope, link pressure, and boundary regret.

Run one summary profile.

Run one trace profile.

Then repeat the test for 8, 16, and 32 tokens.

## 19. Numerical acceptance limits

Compare all output elements with a PyTorch reference.

Use these limits for `f32` output:

```text
absolute tolerance = 1.0e-5
relative tolerance = 1.0e-4
```

Reject a result that contains `NaN` or infinity.

Report the maximum absolute error.

Report the maximum relative error.

Report the index of each maximum error.

Do not use a finite-value check as the only correctness check.

## 20. Performance acceptance limits

The four-token trace must contain zero `digital.matmul_partition` tasks.

The four-token trace must contain zero partition-copy instructions.

The distributed run must retire no more than 55,500,000 instructions.

The distributed run must not exceed 12.109068 ms.

The source tile workspace must not contain one buffer for each operand slice.

The route payload must contain only the selected slice elements.

The route payload must not contain the full base tensor for each shard.

The compiler prediction error must not exceed 10 percent.

Report the first token count that gives a distributed speedup.

## 21. Completion checklist

The change is complete only when all answers are `yes`.

- Does view mode create no partition task?
- Does each view identify one base tensor?
- Does each remote route contain a source byte offset?
- Does each remote route send only the slice bytes?
- Does each local view avoid an allocation?
- Does the scheduler charge the correct route size?
- Does the compiler reject unsafe non-contiguous views?
- Does GPT-2 match the PyTorch reference?
- Does the four-token run meet the runtime limit?
- Do all unit tests use `--verify-each`?
- Do all isolated core modules pass MLIR verification?
- Do all core modules translate without unregistered operations?
- Do all core objects link into RISC-V ELFs?
- Do all active tiles complete in QEMU and SST?

## 22. Work order

Do the work in this order:

1. Add and verify `sculptor.task_graph.slice`.
2. Add `partition-mode=view`.
3. Replace partition tasks with slice operations.
4. Add the contiguous-layout checks.
5. Add the GPT-2 head-major QKV layout.
6. Update task-graph analysis and scoring.
7. Update per-core partitioning.
8. Add `sourceByteOffset` to deployment routes.
9. Update the Golem tile ABI emitter.
10. Add the matching runtime ABI field.
11. Add all unit tests.
12. Run the four-token summary and trace tests.
13. Run the 8, 16, and 32-token tests.
14. Make view mode the default after all tests pass.

Do not combine output-assembly removal with this change.

Do not add scatter-gather DMA in this change.

Do not change mesh timing parameters in this change.

Do not change scheduler parameters during the controlled comparison.
