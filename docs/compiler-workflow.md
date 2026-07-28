# PyTorch to a two-core Golem task graph

This guide follows one concrete model through the compiler. It begins as two
PyTorch `Linear` layers and ends as a two-core, two-array Sculptor task graph
whose same-core tasks have been fused, whose logical-array ABI has been
replaced by fixed array bindings, and whose surviving tensor resources have
been finalized. The task implementations are then lowered to LLVM dialect
while the finalized Sculptor graph remains as structured runtime metadata.

The example is intentionally small. Its purpose is to make every transformation
visible:

```text
PyTorch
   |
   v
Torch-MLIR Linalg
   |
   v
Sculptor layers
   |
   v
MVM operations
   |
   v
8x8 Golem array operations
   |
   v
Callable tasks and dependency graph
   |
   v
Logical placement islands
   |
   v
Two-core snake schedule
   |
   v
Same-core task fusion
   |
   v
Golem runtime shims and task-graph ABI lowering
   |
   v
Runtime tensor-resource finalization
   |
   v
LLVM-dialect task implementations
plus the retained Sculptor task graph
```

The reproducible test is
[`tests/torch-mlir-sculptor/run-test.sh`](../tests/torch-mlir-sculptor/run-test.sh).
After the compiler environment has been built, run it from the repository root:

```bash
./tests/torch-mlir-sculptor/run-test.sh
```

## The model

The source model is
[`tests/torch-mlir-sculptor/model.py`](../tests/torch-mlir-sculptor/model.py):

```python
class TwoLinearLayerApplication(nn.Module):
    def __init__(self):
        super().__init__()
        self.first = nn.Linear(4, 3)
        self.second = nn.Linear(3, 2)

    def forward(self, value):
        return self.second(self.first(value))
```

The tensor shapes flow through the model as follows:

```text
 input             first linear             second linear          output
[1 x 4] -- W0[3 x 4], b0[3] --> [1 x 3] -- W1[2 x 3], b1[2] --> [1 x 2]
```

The test uses fixed weights, biases, and input so that its arithmetic is easy to
check:

```text
x = [1, 2, 3, 4]

     [1, 0, 0, 0]          [ 1]
W0 = [0, 1, 1, 0]     b0 = [-1]
     [0, 0, 0, 1]          [ 2]

W0*x + b0 = [2, 4, 6]

     [1,  1,   1]          [0]
W1 = [2, -1, 0.5]     b1 = [1]

W1*[2, 4, 6] + b1 = [12, 4]
```

PyTorch evaluates this reference computation before compilation:

```text
PyTorch result: [[12.0, 4.0]]
```

## The generated artifacts

The test retains every stage under `build/tests/torch-mlir-sculptor/`:

| Artifact | Meaning |
|---|---|
| `two-linear-linalg.mlir` | Torch-MLIR Linalg-on-tensors import |
| `two-linear-sculptor.mlir` | Canonical Sculptor linear layers |
| `two-linear-extracted.mlir` | Separately outlined layer functions |
| `two-linear-mvm.mlir` | MVM operations plus digital bias work |
| `two-linear-golem.mlir` | Explicit 8x8 array and digital task regions |
| `two-linear-tasks.mlir` | Private callable task functions |
| `two-linear-task-graph.mlir` | Symbolic resources, tasks, and dependencies |
| `two-linear-islands.mlir` | Logical placement-island assignments |
| `two-linear-scheduled.mlir` | Physical two-core and two-array schedule |
| `two-linear-fused.mlir` | Same-core task components fused for execution |
| `two-linear-llvm-shims.mlir` | Runtime shims and corrected task-graph ABI |
| `two-linear-finalized.mlir` | Final tensor slots, task indices, and workspace |
| `two-linear-llvm.mlir` | LLVM task code plus retained Sculptor graph metadata |

The rest of this guide explains what changes between those files.

## 1. Import PyTorch into Torch-MLIR

[`import-model.py`](../tests/torch-mlir-sculptor/import-model.py) uses
`torch_mlir.fx.export_and_import` with the `linalg-on-tensors` output type.
Torch-MLIR represents each `Linear` layer as a weight transpose, matrix
multiplication, and bias addition:

```mlir
%transposed = linalg.transpose ... tensor<3x4xf32> to tensor<4x3xf32>
%matmul = linalg.matmul
    ins(%arg0, %transposed : tensor<1x4xf32>, tensor<4x3xf32>)
    ...
%biased = linalg.generic ... arith.addf ...
```

At this stage the compiler sees general tensor algebra. It does not yet have a
Golem array, a task, an island, or a core assignment.

```text
PyTorch Linear(4, 3)
          |
          v
transpose W0 -> linalg.matmul -> elementwise bias add

PyTorch Linear(3, 2)
          |
          v
transpose W1 -> linalg.matmul -> elementwise bias add
```

## 2. Recover Sculptor layers

The first Sculptor pass is:

```bash
--sculptor-canonicalize-layers
```

It recognizes the Linalg pattern and recovers the higher-level operations:

```mlir
%0 = sculptor.nn.linear %arg0, %weight0, %bias0
    : (...) -> tensor<1x3xf32>
%1 = sculptor.nn.linear %0, %weight1, %bias1
    : (...) -> tensor<1x2xf32>
```

Conceptually, several generic operations collapse back into one meaningful
layer:

```text
transpose + matmul + add          sculptor.nn.linear
          [generic]       --->       [recognized layer]
```

## 3. Extract the layers

The next pass is:

```bash
--sculptor-extract-layers
```

It outlines both layers from `forward`:

```mlir
func.func @forward(%input: tensor<1x4xf32>) -> tensor<1x2xf32> {
  %0 = call @linearwbias_0(%input)
      : (tensor<1x4xf32>) -> tensor<1x3xf32>
  %1 = call @linearwbias_1(%0)
      : (tensor<1x3xf32>) -> tensor<1x2xf32>
  return %1 : tensor<1x2xf32>
}
```

The program now has an explicit layer boundary:

```text
@forward
   |
   +--> @linearwbias_0 -- tensor<1x3xf32> --> @linearwbias_1
                                                        |
                                                        v
                                                 tensor<1x2xf32>
```

This boundary will later become the boundary between two placement islands and
two cores.

## 4. Convert each layer into an MVM

The pass:

```bash
--sculptor-convert-layers
```

lowers each `sculptor.nn.linear` into an analog matrix-vector multiplication
followed by a digital bias addition:

```mlir
%mvm = sculptor.mvm %input, %weight
%result = sculptor.task_region
    kind = "digital.bias_add"
    name = "linear_bias_add"(%mvm) {
  ...
}
```

The division of responsibility is now explicit:

```text
             analog work       digital work
input ------>    MVM    ------>  bias add  ------> result
                  ^
                  |
                weight
```

There are two MVMs: one for the `3x4` weight and one for the `2x3` weight.

## 5. Expand the MVMs into 8x8 Golem arrays

The pass:

```bash
--sculptor-expand-mvm-to-golem="array-rows=8 array-cols=8"
```

turns each abstract MVM into explicit array and digital operations:

```text
                       one expanded linear layer

weights --> [A] matrix setup -------------------+
                                                |
input -----> [D] vector tile --> [A] array MVM -+
                                      |
                                      v
                              [D] tile recombine
                                      |
                                      v
                                 [D] bias add
```

`[A]` is analog work and `[D]` is digital work.

The analog MVM region contains the primitive Golem array sequence:

```mlir
sculptor.array.load %vector, %array
%execution = sculptor.array.execute %array
%result = sculptor.array.store %execution
```

The source matrices are smaller than the physical arrays:

```text
Layer 0                         Layer 1

valid weight: 3 x 4            valid weight: 2 x 3
physical tile: 8 x 8           physical tile: 8 x 8

+----------------+             +----------------+
| V V V V 0 0 0 0|             | V V V 0 0 0 0 0|
| V V V V 0 0 0 0|             | V V V 0 0 0 0 0|
| V V V V 0 0 0 0|             | 0 0 0 0 0 0 0 0|
| 0 0 0 0 0 0 0 0|             | 0 0 0 0 0 0 0 0|
| 0 0 0 0 0 0 0 0|             | 0 0 0 0 0 0 0 0|
| 0 0 0 0 0 0 0 0|             | 0 0 0 0 0 0 0 0|
| 0 0 0 0 0 0 0 0|             | 0 0 0 0 0 0 0 0|
| 0 0 0 0 0 0 0 0|             | 0 0 0 0 0 0 0 0|
+----------------+             +----------------+
```

`V` marks valid model data and `0` marks padding. The IR preserves both facts:

```mlir
sculptor.tile_physical_shape = [8, 8]
sculptor.tile_valid_shape = [3, 4]  // layer 0
sculptor.tile_valid_shape = [2, 3]  // layer 1
```

Because each matrix fits in one array, each layer produces five task regions:

1. matrix setup;
2. vector tiling;
3. array MVM;
4. tile recombination; and
5. bias addition.

The recombination is trivial in this example, but it remains explicit because
larger matrices can span several arrays.

## 6. Materialize callable tasks

The pass:

```bash
--sculptor-materialize-tasks
```

outlines the ten inline task regions into ten private functions. `forward`
becomes a sequence of calls:

```mlir
%array0 = call @task_linearwbias_0_matrix_tile_0_0_0()
%vector0 = call @task_linearwbias_0_vector_tile_0_1(%input)
%mvm0 = call @task_linearwbias_0_mvm_0_0_2(%vector0, %array0)
...
```

Every function carries metadata describing its role:

```mlir
sculptor.task_domain = "analog"
sculptor.task_kind = "sculptor.mvm"
sculptor.task_name = "linearwbias_0_mvm_0_0"
sculptor.source_layer = "linearwbias_0"
sculptor.source_task_ordinal = 2
```

The compiler has now turned anonymous regions of work into named, callable
units that a task graph can reference.

## 7. Assemble the task graph

The pass:

```bash
--sculptor-assemble-task-graph
```

creates `@generate_task_graph`. It declares:

- one model input resource;
- one model output resource;
- nine intermediate resources;
- ten task nodes; and
- the dependencies between those nodes.

The resulting graph is:

```text
                              LAYER 0
                              =======

  Model input [1x4] --> [D] Vector Tile 0 -------+
                                                   v
  Layer 0 weights ----> [A] Matrix Setup 0 ----> [A] MVM 0
                                                   |
                                                   v
                                        [D] Tile Recombine 0
                                                   |
                                                   v
                                           [D] Bias Add 0
                                                   |
                                            value [1x3]
                                                   |
                              LAYER 1             v
                              =======     [D] Vector Tile 1 -------+
                                                                    v
  Layer 1 weights ----> [A] Matrix Setup 1 ---------------------> [A] MVM 1
                                                                    |
                                                                    v
                                                         [D] Tile Recombine 1
                                                                    |
                                                                    v
                                                            [D] Bias Add 1
                                                                    |
                                                                    v
                                                         Model output [1x2]
```

The matrix-setup nodes have no dependencies. They are independent roots, so
both arrays can in principle be prepared before their activation arrives.

The first MVM waits for both matrix setup and vector tiling:

```mlir
%mvm0 = sculptor.task.create ...,
  inputs[%vector0, %array0],
  outputs[%mvm_output0],
  deps[%matrix_setup0, %vector_tile0]
```

The second layer's vector tile explicitly depends on the first layer's bias
addition. That is the cross-layer data-flow edge.

## 8. Build logical placement islands

The pass:

```bash
--sculptor-build-task-graph-islands
```

groups closely related work without choosing physical cores:

```text
+------------------------ ISLAND 0 -------------------------+
| matrix setup 0 -> vector tile 0 -> MVM 0                 |
|                         -> recombine 0 -> bias add 0       |
+--------------------------------+--------------------------+
                                 |
                                 | tensor<1x3xf32>
                                 v
+------------------------ ISLAND 5 -------------------------+
| matrix setup 1 -> vector tile 1 -> MVM 1                 |
|                         -> recombine 1 -> bias add 1       |
+-----------------------------------------------------------+
```

All five first-layer tasks receive:

```mlir
sculptor.schedule.island_id = 0
```

All five second-layer tasks receive:

```mlir
sculptor.schedule.island_id = 5
```

These are stable logical anchor IDs, not core IDs. The second island is `5`
because its matrix-setup anchor begins at task index 5.

## 9. Schedule the islands onto two cores

The pass:

```bash
--sculptor-schedule-task-graph="cores=2 arrays-per-core=1 schedule=snake"
```

maps the logical islands to a `1x2` mesh:

```text
                     12-byte activation transfer
                         tensor<1x3xf32>

       Core 0        -------------------------->        Core 1
    coordinate (0,0)                                coordinate (0,1)

+----------------------+                         +----------------------+
| Island 0             |                         | Island 5             |
|                      |                         |                      |
| Matrix Setup 0       |                         | Matrix Setup 1       |
| Vector Tile 0        |                         | Vector Tile 1        |
| MVM 0                |                         | MVM 1                |
| Tile Recombine 0     |                         | Tile Recombine 1     |
| Bias Add 0           |                         | Bias Add 1           |
|                      |                         |                      |
| Local array:    0    |                         | Local array:    0    |
| Physical array: 0    |                         | Physical array: 1    |
+----------------------+                         +----------------------+
```

With one array on each core, both task groups use local array ID `0`. The
machine-wide physical IDs distinguish them:

| Layer | Island | Core | Local array | Physical array |
|---|---:|---:|---:|---:|
| First linear | 0 | 0 | 0 | 0 |
| Second linear | 5 | 1 | 0 | 1 |

The first layer produces three `f32` values:

```text
3 values x 4 bytes/value = 12 bytes
```

Because the consumer is on core 1, the schedule records:

```mlir
sculptor.schedule.inter_core_transfer_bytes = 12
sculptor.schedule.logical_array_to_analog_array = [0, 1]
sculptor.schedule.num_cores = 2
sculptor.schedule.arrays_per_core = 1
```

For a one-row mesh, the snake ordering is simply core 0 followed by core 1.
The same scheduling policy alternates direction between rows on a larger mesh.

## 10. Fuse same-core tasks

The pass:

```bash
--sculptor-fuse-task-graph
```

combines connected tasks when they belong to both the same island and the same
core. Before fusion, each layer has five graph nodes:

```text
matrix setup --+
               |
vector tile --> MVM --> tile recombine --> bias add
```

The matrix setup remains independent, while the four execution-stage tasks
become one `mixed.fused` task:

```text
matrix setup --+
               |
input --------> mixed.fused --------------------> layer output
                |    |      |         |
                |    |      |         +-- bias add
                |    |      +------------ tile recombine
                |    +------------------- MVM
                +------------------------ vector tile
```

Across the full model, fusion changes the graph from:

```text
10 task nodes
 9 task dependencies
```

to:

```text
 4 task nodes
 3 task dependencies

Core 0: matrix setup 0 --> fused compute 0 --+
                                             | 12-byte transfer
Core 1: matrix setup 1 --> fused compute 1 <-+
```

The resulting graph is equivalent to:

```text
+------------------- Core 0 / Island 0 -------------------+
|                                                         |
|  Matrix Setup 0 ----+                                   |
|                     v                                   |
|  Model input ----> Fused Layer 0                        |
+-------------------------+-------------------------------+
                          |
                          | tensor<1x3xf32> (12 bytes)
                          v
+------------------- Core 1 / Island 5 -------------------+
|                                                         |
|  Matrix Setup 1 ----+                                   |
|                     v                                   |
|  Layer 0 output -> Fused Layer 1 ----> Model output     |
+---------------------------------------------------------+
```

The fused functions keep their original placement:

```mlir
sculptor.task_kind = "mixed.fused"
sculptor.runtime.core_id = 0  // first layer
sculptor.runtime.core_id = 1  // second layer
```

Matrix setup stays separate because it has no activation dependency and can
program an array before compute begins. Fusion also removes the now-redundant
materialized `forward` function. The task graph becomes the live description of
the distributed program.

The core assignments, physical array assignments, and 12-byte inter-core edge
are unchanged.

## 11. Lower Golem operations and the task-graph ABI

The pass:

```bash
--sculptor-lower-golem-to-llvm-shims
```

performs two related transformations. First, it creates declarations for four
runtime entry points:

```mlir
func.func private @golem_analog_mvm_set(memref<8x8xf32>, i32)
func.func private @golem_analog_mvm_load(memref<1x8xf32>, i32)
func.func private @golem_analog_mvm_compute(i32)
func.func private @golem_analog_mvm_store(memref<?x?x?xf32>, i32)
```

The array setup becomes:

```text
8x8 weight memref --> golem_analog_mvm_set(array_id)
```

The MVM becomes:

```text
input memref
    |
    v
golem_analog_mvm_load(array_id)
    |
    v
golem_analog_mvm_compute(array_id)
    |
    v
golem_analog_mvm_store(output memref, array_id)
```

In MLIR:

```mlir
call @golem_analog_mvm_load(%input, %array_id)
call @golem_analog_mvm_compute(%array_id)
call @golem_analog_mvm_store(%output, %array_id)
```

There are two calls to each shim, one for each layer. No `sculptor.array.set`,
`load`, `execute`, or `store` operations remain.

Second, the same pass corrects the callable task-graph ABI. Before lowering,
the graph still represents each programmed analog array as a logical resource:

```text
setup 0 --> logical array 0 --+
                              +--> fused layer 0
model input ------------------+

setup 1 --> logical array 1 --+
                              +--> fused layer 1
activation -------------------+
```

After lowering, those logical resources disappear:

```text
setup 0 ---------------- dependency ----------------+
                                                    v
model input --------------------------------> fused layer 0
                                                    |
                                                    | activation
                                                    v
setup 1 ---------------- dependency --------> fused layer 1
                                                    |
                                                    v
                                               model output
```

The setup tasks no longer produce runtime values:

```mlir
%setup0 = sculptor.task.create ...,
  inputs[], outputs[], deps[]
  {
    sculptor.runtime.local_array_id = 0,
    sculptor.runtime.physical_array_id = 0
  }
```

Each fused task accepts only its dynamic tensor input. The fixed array
selection remains in task metadata and in the constant passed to the shim:

```mlir
%fused0 = sculptor.task.create ...,
  inputs[%model_input],
  outputs[%activation],
  deps[%setup0]
  {
    sculptor.runtime.local_array_id = 0,
    sculptor.runtime.physical_array_id = 0
  }
```

The second layer has the corresponding shape:

```mlir
%fused1 = sculptor.task.create ...,
  inputs[%activation],
  outputs[%model_output],
  deps[%fused0, %setup1]
```

The graph-level `sculptor.schedule.logical_array_to_analog_array = [0, 1]`
mapping remains only as placement provenance for reporting and visualization.
It is no longer represented by a task resource or runtime slot.

This is the compiler/runtime boundary. The IR has changed from describing a
Golem array abstractly to calling functions that the Golem runtime can
implement with the target's custom operations, and the task graph now matches
those functions' tensor-only dynamic ABI.

## 12. Finalize surviving tensor resources

The pass:

```bash
--sculptor-finalize-task-graph-resources
```

runs after logical-array ABI lowering because that lowering changes the
resource topology. It assigns indices to the final four tasks and slots only
to the three surviving tensor resources:

```text
                         FINAL RUNTIME GRAPH

  task 0: setup array 0 ---------------- dependency ----------------+
                                                                    |
  slot 0: model input, 16 bytes --------------------------+         |
                                                          v         v
                                                    task 1: fused layer 0
                                                          |
                                                          v
                                         slot 2: activation [1x3],
                                                 12 bytes
                                                          |
  task 2: setup array 1 ---------------- dependency ------+
                                                          |
                                                          v
                                                    task 3: fused layer 1
                                                          |
                                                          v
                                         slot 1: model output, 8 bytes
```

The finalized relationships are:

| Task index | Work | Input slots | Output slots | Dependencies |
|---:|---|---|---|---|
| 0 | Matrix setup 0 | `[]` | `[]` | `[]` |
| 1 | Fused layer 0 | `[0]` | `[2]` | `[0]` |
| 2 | Matrix setup 1 | `[]` | `[]` | `[]` |
| 3 | Fused layer 1 | `[2]` | `[1]` | `[1, 2]` |

The resource table now contains only tensors:

| Slot | Resource | Byte size |
|---:|---|---:|
| 0 | Model input `tensor<1x4xf32>` | 16 |
| 1 | Model output `tensor<1x2xf32>` | 8 |
| 2 | Inter-core activation `tensor<1x3xf32>` | 12 |

The graph records:

```mlir
sculptor.runtime.input_slots = [0]
sculptor.runtime.output_slots = [1]
sculptor.runtime.resource_count = 3
sculptor.runtime.temp_base_slot = 2
sculptor.runtime.temp_count = 1
sculptor.runtime.temp_offsets = [0]
sculptor.runtime.workspace_size = 12
```

The activation is the only temporary runtime payload. Array initialization and
selection are represented entirely by setup dependencies and fixed array
bindings.

## 13. Lower task implementations to LLVM dialect

The remaining standard MLIR passes lower the implementations of the four
runtime tasks:

```bash
--canonicalize
--cse
--empty-tensor-to-alloc-tensor
--one-shot-bufferize="bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map"
--convert-bufferization-to-memref
--convert-linalg-to-loops
--lower-affine
--convert-scf-to-cf
--convert-math-to-llvm
--expand-strided-metadata
--lower-affine
--convert-arith-to-llvm
--convert-index-to-llvm
--convert-cf-to-llvm
--finalize-memref-to-llvm
--convert-func-to-llvm
--reconcile-unrealized-casts
```

The sequence has four broad jobs:

```text
tensor values
    |
    | empty-tensor-to-alloc-tensor
    | one-shot-bufferize
    v
memrefs and explicit allocation
    |
    | linalg-to-loops
    | affine/scf-to-cf
    v
loops and control-flow blocks
    |
    | arith/index/cf/memref/func-to-llvm
    v
LLVM functions, pointers, descriptors, branches, and calls
```

The resulting module is deliberately hybrid:

```text
module
|
+-- LLVM task implementation code
|   |
|   +-- llvm.mlir.global       weight and bias constants
|   +-- llvm.func              four task functions
|   +-- llvm.call              malloc, free, and Golem shims
|   +-- llvm.br/llvm.cond_br   lowered loops and control flow
|   +-- LLVM pointer/struct    lowered memref descriptors
|
+-- Sculptor runtime metadata
    |
    +-- func.func @generate_task_graph
        +-- five finalized resources
        +-- four task nodes
        +-- task indices and slots
        +-- core, array, and island assignments
```

The task implementations no longer contain operations from the `tensor`,
`linalg`, `bufferization`, `memref`, `affine`, `scf`, `cf`, `arith`, or `math`
dialects. Their memref arguments have become the pointer, offset, size, and
stride fields of LLVM-compatible descriptors. For example, a conceptual call:

```mlir
call @golem_analog_mvm_load(%input_memref, %array_id)
```

becomes an LLVM call carrying the expanded descriptor:

```mlir
llvm.call @golem_analog_mvm_load(
    %allocated_ptr, %aligned_ptr, %offset,
    %size0, %size1, %stride0, %stride1,
    %array_id)
```

The task graph is not ordinary compute code, so these standard lowering passes
do not erase it. Keeping `@generate_task_graph` structured allows a later
packaging or export stage to read the finalized slots, dependencies, and
placements while the functions it names are already in LLVM dialect.

This stage therefore reaches LLVM-compatible task code, not yet one standalone
LLVM module or executable. Per-core packaging still needs to consume the graph,
select the functions for each tile, and emit or preserve the runtime metadata
needed to launch them.

## The complete pass sequence

The test writes one artifact after every pass to make the transformation easy
to inspect. The same final IR can be generated in one invocation:

```bash
install/sculptor-mlir/bin/sculptor-mlir-opt \
    build/tests/torch-mlir-sculptor/two-linear-linalg.mlir \
    --sculptor-canonicalize-layers \
    --sculptor-extract-layers \
    --sculptor-convert-layers \
    --sculptor-expand-mvm-to-golem="array-rows=8 array-cols=8" \
    --sculptor-materialize-tasks \
    --sculptor-assemble-task-graph \
    --sculptor-build-task-graph-islands \
    --sculptor-schedule-task-graph="cores=2 arrays-per-core=1 schedule=snake" \
    --sculptor-fuse-task-graph \
    --sculptor-lower-golem-to-llvm-shims \
    --sculptor-finalize-task-graph-resources \
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
    -o build/tests/torch-mlir-sculptor/two-linear-llvm.mlir
```

The staged and one-shot forms produce the same task functions, shim calls,
globals, and finalized graph. Their printed global and dialect-resource order
may differ because those symbols are emitted from unordered internal
collections; that textual ordering has no semantic effect.

## What this example proves

The test proves that the pinned compiler stack can:

- import a real PyTorch module through Torch-MLIR;
- recover two neural-network layers from generic Linalg;
- turn both layers into analog MVM plus digital work;
- map the weight tensors into uniform 8x8 arrays while retaining valid shapes;
- create named tasks, data resources, and explicit dependencies;
- group those tasks into placement islands;
- place the two layers on two cores and two physical arrays;
- account for the 12-byte activation crossing the core boundary;
- fuse each layer's same-core execution tasks without crossing that boundary;
- replace Golem operations with runtime shims while eliminating logical-array
  resources from the callable task-graph ABI;
- finalize three tensor resources and a 12-byte temporary workspace; and
- lower the four task implementations to LLVM dialect while retaining the
  finalized graph.

It does not yet prove execution of the final scheduled program. The remaining
work is to package or dispatch the LLVM task functions per core, translate and
compile the resulting per-core LLVM modules, link the Golem runtime shim
implementations, and execute the distributed result through QEMU and SST. The
current output combines the compiler's complete logical and physical plan with
LLVM-compatible implementations of its tasks.
