# Testing

Each complete system scenario owns its sources, SST configuration when
needed, and a foreground `run-test.sh`.

## Torch-MLIR compiler

```bash
./tests/torch-mlir/run-test.sh
```

This runs the installed `torch-mlir-opt` against the pinned branch's
Torch-to-Linalg matrix-operation test. It verifies diagnostics across every
split input and requires the converted output to contain `linalg.matmul`.

## PyTorch single-core executable

```bash
./tests/pytorch-single-core/run-test.sh
```

This is the end-to-end compiler proof. It starts with an ordinary
`torch.nn.Module` containing two deterministic `torch.nn.Linear` layers. The
runner uses `torch.export` and Torch-MLIR's FX importer, lowers the resulting
Torch program to Linalg-on-tensors, bufferizes it, lowers it through the MLIR
LLVM dialect, translates it to LLVM IR, and compiles it into a freestanding
RISC-V ELF with the pinned Golem Clang.

The first layer maps four inputs to three outputs, and the second maps those
three values to two outputs. The ELF runs directly on one QEMU RISC-V hart.
Its bare-metal driver passes input and output ranked memref descriptors to the
generated C-interface wrapper and requires the result:

```text
[12, 4]
```

The proof contains no hand-written Torch or Linalg IR, Linux runtime, dynamic
loader, or host-side execution of the lowered function. PyTorch runs only
during compilation to export the model; the numerical result checked at the
end is produced by the generated RISC-V code.

## Torch-MLIR to Sculptor layer extraction

```bash
./tests/torch-mlir-sculptor/run-test.sh
```

See [PyTorch to a two-core Golem task graph](compiler-workflow.md) for a
stage-by-stage walkthrough of this test, its generated IR, task graph, logical
islands, physical schedule, and runtime shims.

This compiler test starts with a deterministic PyTorch module containing
`Linear(4, 3)` followed by `Linear(3, 2)`. Torch-MLIR produces two
`linalg.matmul` computations and their bias additions. Sculptor canonicalizes
those regions to two `sculptor.nn.linear` operations, then
`sculptor-extract-layers` outlines both into separate `linear_w_bias`
functions. The test then runs `sculptor-convert-layers` and expands both MVMs
to Golem operations for 8x8 arrays. Finally, `sculptor-materialize-tasks`
outlines the ten generated task regions into callable task functions, and
`sculptor-assemble-task-graph` creates their symbolic resource and dependency
graph. `sculptor-build-task-graph-islands` then groups the graph into two
logical five-task placement islands, one for each linear layer, without
assigning physical cores or arrays. A two-core snake schedule with one analog
array per core places the first island on core 0 and physical array 0, and the
second on core 1 and physical array 1. It records the three-float intermediate
activation as a 12-byte inter-core transfer. `sculptor-fuse-task-graph`
combines each layer's same-core vector tiling, MVM, recombination, and bias work
into one `mixed.fused` task while retaining each matrix setup as an independent
task. This reduces the scheduled graph from ten tasks to four without removing
the inter-core edge. `sculptor-lower-golem-to-llvm-shims` replaces each
scheduled Golem array operation with its corresponding `golem_analog_mvm_*`
runtime shim and lowers the task-graph ABI: logical-array resources disappear,
fixed local and physical array bindings remain, and setup ordering becomes
explicit dependencies. `sculptor-finalize-task-graph-resources` then assigns
the four task indices and three surviving tensor resource slots, including a
12-byte temporary workspace for the inter-core activation. The
remaining standard lowering sequence bufferizes the task implementations and
converts their tensor computation, loops, control flow, arithmetic, memory
operations, functions, and shim calls to LLVM dialect. The finalized Sculptor
task graph deliberately remains alongside that LLVM task code as structured
runtime metadata. The complete post-extraction IR is printed and retained under
`build/tests/torch-mlir-sculptor/two-linear-extracted.mlir`; the converted and
expanded artifacts are `two-linear-mvm.mlir` and `two-linear-golem.mlir`.
The final stages are retained as `two-linear-tasks.mlir` and
`two-linear-task-graph.mlir`, followed by `two-linear-islands.mlir`, in the
same directory. The physical placement is retained as
`two-linear-scheduled.mlir`, the fused graph as `two-linear-fused.mlir`, and
the corrected shim/task-graph ABI as `two-linear-llvm-shims.mlir`. The final
three-resource runtime layout is `two-linear-finalized.mlir`, and the LLVM task
implementations plus retained graph are `two-linear-llvm.mlir`.

## Generated two-core Sculptor runtime

```bash
./tests/sculptor-core-elf/run-test.sh
```

This is the first complete compiler-to-mesh execution proof. It takes the same
two-linear PyTorch model, partitions the scheduled graph, and extracts active
cores 0 and 1. Each isolated core is finalized independently, lowered to LLVM
dialect, and passed through `sculptor-emit-golem-tile-abi`. That pass emits the
core ID plus immutable boot-task, dispatch-task, route, and model-I/O tables.
Each result becomes an independent bare-metal RISC-V ELF linked with
`libgolem-runtime.a`.

SST launches both ELFs as a 2x1 mesh with one private 8x8 analog array per
tile. The basic runtime on each tile:

1. validates its generated ABI tables;
2. runs its matrix-setup boot task exactly once;
3. dispatches its compiled compute task by global task ID; and
4. sends or receives the generated route as independent 32-bit words.

Core 1 sends READY only after its matrix is installed. Core 0 then evaluates
the first layer:

```text
[1,2,3,4] -- task 1 --> [2,4,6]
```

The three activation floats cross the mesh as exactly three 32-bit transfers.
Together with READY and DONE, the runner verifies five total 32-bit words and
no wider transfer. Core 1 evaluates task 3 and checks the complete model result:

```text
[2,4,6] -- task 3 --> [12,4]
```

The same test can replace only the scheduled MVM implementations with digital
RISC-V matmuls while preserving placement, routing, tensor shapes, and inputs:

```bash
SCULPTOR_CORE_MVM_EXECUTION=digital \
./tests/sculptor-core-elf/run-test.sh
```

The analog and digital variants both produce `[12,4]`. Under the current
single-issue 1 GHz CPU model, the analog variant completes in 5.502 us and the
digital variant in 5.915 us. This small 8x8 comparison validates functional
equivalence and the compiler/runtime path; it is not representative of
large-matrix accelerator speedup.

This initial runtime deliberately supports one fixed execution and
application-supplied ranked-memref descriptors. It does not yet infer tensor
shapes from the generated tables, multiplex packet streams, or schedule
multiple in-flight executions.

## Four-layer generated 2x2 mesh

```bash
./tests/sculptor-four-layer-mesh/run-test.sh
```

This test scales the complete compiler-to-simulator path from two layers and
two active cores to four layers and four active cores. Its PyTorch model
contains four deterministic `Linear(4, 4)` layers and produces:

```text
[1,2,3,4]
  -> layer 0: [2,3,4,5]
  -> layer 1: [4,7,10,13]
  -> layer 2: [12,9,6,3]
  -> layer 3: [21,10,20,15]
```

Sculptor initially materializes twenty tasks. Same-core fusion reduces those
to four matrix-setup tasks and four compute tasks. A four-core, one-array-per-
core snake schedule places the layers along the 2x2 path:

```text
core 0 (layer 0) ----> core 1 (layer 1)
                              |
                              v
core 2 (layer 3) <---- core 3 (layer 2)
```

The test partitions and extracts all four cores, emits a separate tile ABI and
bare-metal RISC-V ELF for each, and gives every tile one private 8x8 analog
array. Before core 0 admits the model input, READY messages confirm that all
four generated boot tables have installed their matrices.

Each inter-layer activation contains four `float32` elements. The three
compiler-generated routes therefore carry twelve 32-bit activation words.
Three READY words and one DONE word bring total delivered traffic to sixteen
32-bit words. The runner also verifies seventeen physical word-hops: twelve
for the three direct-neighbor activation routes and five for the boot/completion
control traffic.

The expected final output is checked on core 2. The historical run at 1 GHz
with eight-cycle analog compute latency and the native analog backend had the
following synchronized SST completion time:

```text
7.286 us
```

This proof uses the same `BasicTileRuntime` as the two-layer example; it does
not introduce a model-specific runtime or handwritten compute task.

The same generated model can be run with one independent private L1 attached
to every tile:

```bash
MITTENS_MEMORY_BACKEND=memhierarchy \
    ./tests/sculptor-four-layer-mesh/run-test.sh
```

This is the first complete model-level memory-backend regression. It keeps
QEMU as the owner of each tile's functional RAM while sending every guest
data load/store through that tile's own 32 KiB, four-way, 64-byte-line SST
cache. The reference run preserves the exact model output, observes 4,923
timed L1 accesses across the four tiles (4,773 hits and 150 misses, a 96.95%
hit rate), and completes in `18.559 us`.

## Eight-layer generated 2x2 dual-array mesh

```bash
./tests/sculptor-eight-layer-mesh/run-test.sh
```

This test doubles the four-layer proof's model depth without increasing the
mesh dimensions. The PyTorch model contains eight deterministic
`Linear(4, 4)` layers. Sculptor lowers and fuses the graph into eight matrix
setup tasks and eight compute tasks, then assigns two consecutive layers to
the two local analog arrays on each tile:

```text
core 0 (layers 0,1) ----> core 1 (layers 2,3)
                                  |
                                  v
core 2 (layers 6,7) <---- core 3 (layers 4,5)
```

The compiler-generated physical-array placement is:

```text
layer:          0  1  2  3  4  5  6  7
core:           0  0  1  1  3  3  2  2
local array:    0  1  0  1  0  1  0  1
physical array: 0  1  2  3  6  7  4  5
```

Each generated ELF boots two matrices and registers two compute tasks. The
runtime obtains the local execution order from the generated incoming and
outgoing route task IDs. It performs a local tensor handoff between the two
tasks and uses the generated route table only at tile boundaries. Consequently
the eight-layer graph still has three inter-core activation routes and carries
the same twelve activation words as the four-layer proof.

The final result on core 2 is:

```text
[56,42,42,60]
```

This exactly matches eager PyTorch. Under the historical 1 GHz,
eight-cycle-latency configuration used by the recorded four-layer proof, the
synchronized SST completion time was:

```text
13.916 us
```

The test proves that a generated core module and ELF can contain multiple
boot-time analog-array initializers and multiple indexed compute tasks. It
does not yet overlap the two arrays: the model dependencies require each
layer to execute after the previous layer.

## Eight-layer generated 4x2 mesh

```bash
./tests/sculptor-eight-layer-mesh-4x2/run-test.sh
```

This test runs the same eight-layer PyTorch model on a four-column,
two-row mesh with one analog array per tile. Sculptor assigns one layer to
each core in snake order:

```text
core 0 ----> core 1 ----> core 2 ----> core 3
                                                |
                                                v
core 4 <---- core 5 <---- core 6 <---- core 7

layer:          0  1  2  3  4  5  6  7
core:           0  1  2  3  7  6  5  4
physical array: 0  1  2  3  7  6  5  4
```

The test partitions the compiler output into eight independent bare-metal
RISC-V ELFs. Every ELF contains one boot-time matrix setup task, one
registered compute task, and its generated incoming and outgoing route
records. Core 0 waits until all eight matrices are installed before admitting
the model input. Core 4 validates the final result:

```text
[56,42,42,60]
```

All seven layer boundaries cross direct-neighbor links. The network therefore
carries twenty-eight 32-bit activation words, seven READY words, and one DONE
word. The test verifies 1,152 delivered bits and 1,440 physical-link bits,
with every individual transfer exactly 32 bits.

Under the same timing configuration as the 2x2 dual-array test, the measured
completion times are:

| Placement | Inter-core activation bytes | Completion |
| --- | ---: | ---: |
| 2x2, two arrays per core | 48 | 13.916 us |
| 4x2, one array per core | 112 | 14.299 us |

The 4x2 placement is 0.383 us slower in this specific test. Both placements
execute the same eight analog computations sequentially because every layer
depends on its predecessor. The small difference reflects their different
generated runtime instruction paths, local task handoffs, synchronization,
and mesh traffic. It must not be interpreted as a general throughput result
or evidence of parallel layer execution.

## ResNet-18 generated 8x8 mesh

```bash
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

This deployment runs a compiled ResNet-18 across 19 active tiles in an 8x8
mesh. Every active tile has a separate bare-metal RISC-V ELF and four
1024x512 analog arrays. The remaining 45 tiles boot a minimal idle image so
the complete physical mesh remains present. The test requires all 19 active
tiles to pass and checks final top-1 class 620.

The original polling runtime completed in 2.092070633 simulated seconds. The
event-driven `RX_WAIT` runtime completed in 1.961999594 seconds. Receive DMA
now completes the same deployment in 1.278562198 seconds: 34.833718% faster
than the blocking-receive baseline and 38.885324% faster than the original
polling run, with the same top-1 result and exactly the same mesh-word count.
See
[ResNet-18 8x8 performance profile](resnet18-profile.md) for the network,
analog, generated-task, and deployment-runtime breakdown; measurement
caveats; and the opt-in profiling commands.

The default physical mesh link is 32 bits at 1 GHz. Width experiments preserve
the 32-bit NIC and tensor word ABI:

```bash
MITTENS_RESNET18_MESH_LINK_WIDTH_BITS=64 \
MITTENS_RESNET18_MESH_LINK_CLOCK=1GHz \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

The synchronized RISC-V CPU clock is independently configurable. This changes
the SST duration charged per retired instruction without changing the QEMU
binary, mesh clock, analog-link clock, or receive-DMA clock. Dynamic
instruction count can still vary slightly because transmit-backpressure
retries depend on relative CPU/device timing:

```bash
MITTENS_RESNET18_CPU_CLOCK=3GHz \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

With only the CPU changed from 1 GHz to 3 GHz, the full test passes 19/19
tiles with top-1 class 620 and completes in 0.431314372388 simulated seconds,
a 2.964339x speedup. All network, receive-DMA, and analog-operation counts
remain unchanged.

The scalar issue width is also independently configurable:

```bash
MITTENS_RESNET18_CPU_ISSUE_WIDTH=2 \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

On the vectorized ResNet-18 artifacts, the 1 GHz dual-issue run passes all
19 active tiles with top-1 class 620 and completes in 0.515424 seconds,
compared with 1.02133 seconds for the matching single-issue vector run. That
is a 1.9815x modeled speedup. Vector instructions retain a separate
one-per-cycle issue limit.

Task tracing is opt-in and records globally synchronized SST timestamps
without modifying mesh packets:

```bash
MITTENS_RESNET18_TASK_TRACE=1 \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

The traced run validates a matched start/finish pair for every task and
produces `task-trace.csv`, `task-trace-summary.csv`, and
`task-trace-gaps.csv` in the deployment build directory. Raw events are
written to one file per tile so concurrent QEMU UART output cannot corrupt
the trace.

For a joined system profile with task, route, network, receive-DMA, analog,
memory, and wait attribution, use:

```bash
MITTENS_RESNET18_PROFILE_MODE=trace \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

The runner writes raw per-tile data below
`performance-profile-raw/` and the joined report below
`performance-profile/` in the selected deployment directory. Use
`MITTENS_RESNET18_PROFILE_MODE=summary` when only unperturbed finish counters
are required.

## GPT-2 scheduling and execution sweep

```bash
./tests/sculptor-gpt2-scheduling-sweep/list-configurations.sh
./tests/sculptor-gpt2-scheduling-sweep/build-test.sh
./tests/sculptor-gpt2-scheduling-sweep/run-test.sh
```

This controlled experiment compiles the GPT-2-small-shaped fixture at static
token lengths 4, 8, 16, and 32. Each graph is placed with fourteen scheduling
configurations and lowered once to native analog MVM execution and once to
digital RISC-V matmuls, producing 112 deployments.

The placement matrix contains fixed-seed random and snake baselines; Greedy
and timing-aware Greedy with single-path lookahead 2 and 3; beam-width-8
Greedy and timing-aware Greedy; and controlled beam-8 variants with directed
link-pressure scoring, width-2 balanced reductions, or both. Beam-width-8
rows are not labeled with a lookahead depth because Sculptor uses beam search
instead of recursive lookahead when the beam width exceeds one.

Every run uses the same 8x8 mesh, four 1024x512 arrays per tile, 1 GHz clock,
dual-issue scalar model, 32-bit mesh links, native QEMU memory, and random
seed zero. Simulations run in the foreground and are resumable. Each
deployment has isolated compiler objects, ELFs, logs, and pass/failure marker.
The build and simulation runners print a resume-aware `[current/total]`
counter plus completed, failed, remaining, and elapsed totals before every
selected deployment.
The consolidated `results.csv` records all 112 expected rows, including
not-yet-run states, Sculptor graph metrics, SST simulated time, output
signature, active cores, retired instruction and modeled cycle totals,
transmitted 32-bit words, and analog-active cycles.

The exact manifest, artifact layout, subset controls, and compiler pass order
are documented in
[`../tests/sculptor-gpt2-scheduling-sweep/README.md`](../tests/sculptor-gpt2-scheduling-sweep/README.md).

One compiled GPT-2 deployment can be profiled with:

```bash
MITTENS_GPT2_PROFILE_MODE=trace \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

The corresponding low-overhead counter mode is
`MITTENS_GPT2_PROFILE_MODE=summary`.

## Animated mesh activity

```bash
MITTENS_VISUALIZATION_EXPORT=1 \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

The visualization flag is disabled by default. When enabled, it selects trace
profiling, joins the per-tile activity, and emits one compact event for every
task interval, logical tensor route, receive-DMA transfer, analog operation,
and nonzero wait. It intentionally does not emit individual 32-bit transfers
or per-cycle router events.

The output is `<deployment>/visualization/trace.json`. Serve the repository
and open the synchronized 8x8 mesh/timeline viewer with:

```bash
./visualizer/serve.sh 8000 \
  "$PWD/build/tests/sculptor-gpt2-8x8/deployment/visualization/trace.json"
```

The same flag works with the ResNet-18 deployment runner. Exact route endpoints
and SST timestamps are preserved; intermediate packet positions are
reconstructed along deterministic XY paths for an understandable flow view.
Because task markers are guest-visible MMIO, visualization runs are
diagnostic and must not replace untraced performance measurements.

## Runtime library

```bash
./runtime/tests/run-test.sh
./tests/runtime-library/run-test.sh
./tests/deployment-runtime-pair/run-test.sh
```

The first command runs host-side unit tests for tensor validation, task
registry lookup, task-instance allocation, ready-queue wraparound, generated
tile-ABI validation, basic boot/dispatch/route behavior, 32-bit transport
callbacks, and backpressure behavior. It also runs a reciprocal framed-route
regression with one bounded cell per endpoint, proving simultaneous
bidirectional sends make receive progress instead of deadlocking, plus a
two-source receive-DMA regression that validates descriptor registration,
completion, and task readiness. The second builds `libgolem-runtime.a` with
the pinned RISC-V compiler, links it into a
freestanding tile ELF, boots that ELF in QEMU, executes a registered scalar
task, and requires `Golem runtime library: PASS`. The third builds two
deployment-runtime ELFs, transfers a framed 300-word tensor through QEMU,
Mittens, and Merlin, and requires the destination to enter a real
`NIC_RECEIVE_WAIT` fd-41 stop before the burst arrives. That two-tile proof
wires the receive-DMA callbacks and checks the exact 300-word result.
At the default 256-bit local width it also requires exactly
`8 + ceil(300 * 32 / 256) = 46` receive-DMA cycles. Running it with
`MITTENS_DEPLOYMENT_RX_DMA_WIDTH_BITS=32` requires 308 cycles and increases
the end-to-end result by exactly 262 ns while preserving payload and result.

All three proofs are available together as `./bootstrap.sh test-runtime`.

## Single tile

```bash
./tests/hello/run-test.sh
```

This builds one bare-metal ELF, boots it directly with the repository QEMU,
prints `Golem Platform v0.1: single tile booted`, and exits successfully.

## RISC-V Vector execution

```bash
./tests/riscv-vector/run-test.sh
```

This builds one bare-metal ELF with explicit
`-march=rv64gcv_xgolemanalog`, then launches it as an SST-managed QEMU tile
with RVV 1.0, `VLEN=256`, and `ELEN=64`. The guest reads `vlenb`, configures
eight active float32 elements with `vsetvli`, loads two complete vector
registers, executes `vfadd.vv`, stores the result, and checks all eight
values. It requires:

```text
RISCV_VECTOR_PASS: RVV 1.0 VLEN=256 floating-point vector add
```

The test deliberately uses explicit RVV assembly. It proves QEMU vector
decode, vector register state, vector floating-point execution, bare-metal
`mstatus.VS` initialization, and SST-to-QEMU CPU configuration without
depending on LLVM's auto-vectorization profitability decisions.

## CPU throughput timing

```bash
./tests/cpu-timing-validation/run-test.sh
```

This gate executes fixed marker-to-marker regions containing an empty
baseline, exactly 1,024 scalar `addi` instructions, and exactly 1,027 RVV
instructions. It runs issue widths 1, 2, and 4 at QEMU synchronization quanta
37 and 1,000. All 18 comparisons must satisfy:

```text
cycles = max(ceil(retired instructions / issue width),
             retired vector instructions)
```

The exact reference results are 1,030/515/258 cycles for the scalar region
and 1,035/1,027/1,027 cycles for the RVV region at widths 1/2/4. Every
measurement must be identical across both host quanta. Machine-readable
results are written to `build/tests/cpu-timing-validation/results.csv`.

## Analog custom instructions

```bash
./tests/analog-instructions/run-test.sh
```

This builds one bare-metal ELF with the pinned `golem-analog` Clang and
executes `mvm.set`, `mvm.l`, `mvm`, `mvm.s`, and `mvm.mv` through the custom
QEMU decoder and shared analog bridge. The guest programs two 4x4 arrays,
submits both command streams before joining, validates both results, moves one
array's output directly into the other, and validates the second computation.
The same ELF must pass once with the native C++ backend and once with CrossSim.

### Focused operation and output proof

```bash
./tests/analog-ops/run-test.sh
```

This is a separate one-tile test with no `networkIF`, Merlin router, or mesh.
It uses one tile-local analog array and reports and requires success for
`mvm.set`, `mvm.l`, `mvm`, and `mvm.s`. The array computes and returns:

```text
[0.1875, -0.15625, -0.0625, -0.125]
```

The guest rejects a nonzero operation status, NaN, or value outside the
documented tolerance. The runner requires all five status/output checks and
runs the same ELF against both the native and CrossSim backends. Both runs
must traverse the fd 41 grant/yield protocol and produce the same synchronized
instruction count and simulated completion time.

## 2x2 analog mesh pipeline

```bash
./tests/analog-mesh-2x2/run-test.sh
```

This test launches four QEMU-backed tiles in a 2x2 Merlin mesh. Every tile owns
one 4x4 analog array and executes `mvm.set`, `mvm.l`, `mvm`, and `mvm.s`.
Four-word float32 vectors follow the serpentine computation order:

```text
tile 0 (0,0) ----> tile 1 (1,0)
     ^                   |
     |                   v
tile 2 (0,1) <---- tile 3 (1,1)
```

The forward computation order is `0 -> 1 -> 3 -> 2`. Tile 2 returns the final
four-word vector to tile 0 for validation and clean simulation completion.
The successive local analog outputs are:

```text
tile 0: [ 0.1875,  -0.15625, -0.0625,   -0.125   ]
tile 1: [ 0.09375, -0.078125,-0.03125,  -0.0625  ]
tile 3: [-0.0625,  -0.03125, -0.078125,  0.09375 ]
tile 2: [-0.03125, -0.015625,-0.0390625, 0.046875]
```

Every tile checks its incoming and computed vectors. The runner executes the
complete mesh with both native and CrossSim backends and verifies Merlin
statistics showing exactly four 32-bit packets on each physical pipeline link.
The two numerical backends must also produce the same synchronized SST
completion time.

## 2x2 analog route comparison

```bash
./tests/analog-route-2x2/run-test.sh
```

This paired test compares `0 -> 1 -> 3 -> 2` with
`0 -> 3 -> 1 -> 2`. Both variants execute the same four sequential MVM
stages, use the same diagonal 0.5 matrix on every tile, and validate the same
final vector. Only the destination sequence changes.

```text
logical route 0132:  0 -> 1 -> 3 -> 2
physical hops:             1    1    1

logical route 0312:  0 -> 3 -> 1 -> 2
physical hops:             2    1    2
```

With deterministic X-then-Y routing, `0132` traverses three physical links
per 32-bit word while `0312` traverses five. The runner verifies 12 versus 20
physical 32-bit word-hops, uses a ten-instruction receive-polling quantum, and
reports both synchronized SST completion times and their difference.

## 2x2 dual-array analog mesh

```bash
./tests/analog-mesh-2x2-dual-array/run-test.sh
```

This extends the 2x2 proof to two independently programmed 4x4 arrays on every
tile and eight MVM stages:

```text
tile 0 array 0 -> tile 1 array 0 -> tile 3 array 0 -> tile 2 array 0
                                                               |
                                                        tile-local RAM
                                                               |
tile 0 array 1 <- tile 1 array 1 <- tile 3 array 1 <- tile 2 array 1
```

The resulting tile sequence is:

```text
0 -> 1 -> 3 -> 2 -> 2 -> 3 -> 1 -> 0
```

The repeated tile 2 is a local handoff rather than a network packet. Tile 2
stores array 0's output into private guest RAM and loads those same four words
into array 1. The test deliberately does not use `mvm.mv`.

All eight stages validate their input and output vectors. The final output at
tile 0 array 1 is:

```text
[-0.0078125, 0.01171875, -0.009765625, -0.00390625]
```

The runner passes against both native and CrossSim backends. It also checks
that each of the six physical mesh hops carries exactly four 32-bit packets;
the local array handoff carries no mesh traffic.

## Two-tile mesh

```bash
./tests/mesh-pair/run-test.sh
```

This builds distinct tile images, launches two QEMU processes through
`mittens.tile`, sends the float32 bit pattern for `3.25` through Merlin, and
returns an acknowledgment to tile 0.

## 3x3 routed mesh

```bash
./tests/mesh-3x3/run-test.sh
```

This launches nine independent QEMU tiles, each attached to its own Merlin
router. Tile 0 at `(0,0)` sends the float32 bit pattern for `3.25` to tile 8
at `(2,2)`. Merlin routes the packet east, east, south, south; the
acknowledgment returns west, west, north, north. Tiles 1 through 7 remain
resident until tile 0 sends explicit shutdown packets.

The runner checks Merlin's per-port packet statistics for both four-hop paths
and fails if any expected directional link was not used. Generated statistics
are written to `build/tests/mesh-3x3/router-statistics.csv`.

## Isolated network timing

```bash
./tests/network-timing-validation/run-test.sh
```

This removes QEMU, the runtime, receive DMA, and application instructions from
the measurement interval. SST-only endpoints inject at one exact cycle and
record both Merlin head arrival and full-packet completion.

The 13 trials cover 1, 8, 64, and 512-word serialization, 1/2/4-hop paths,
and 1/2/4 simultaneous sources contending for one destination, yielding 21
packet observations. With 32-bit, 1 GHz links and 10 ns link latency, every
observation must exactly match:

```text
head = 35 + 12 * (hops - 1)
completion = head + words - 1
```

For one-hop incast, each earlier equal-size contender adds exactly `words`
cycles. The test also verifies that all contenders injected simultaneously
and retains raw receipts, router statistics, logs, and the joined
predicted-versus-measured CSV under
`build/tests/network-timing-validation/`.

## End-to-end analog timing

```bash
./tests/analog-timing-validation/run-test.sh
```

This gate sends `set`, `load`, `execute`, and `store` instructions from a
bare-metal RISC-V guest through QEMU's fd 43 analog bridge and fd 41
synchronization boundary into SST. A single-array case checks exact service
cycles, and a dual-array case additionally checks shared 256-bit-link
serialization and independent compute-engine overlap. The expected timeline
is generated by an independent reference scheduler from the observed command
arrival cycles, so guest instruction time is excluded from the comparison.

See [`tests/analog-timing-validation/README.md`](../tests/analog-timing-validation/README.md)
for the acceptance contract and generated artifacts.

## Producer–MVM–recombine placement distance

```bash
./tests/producer-mvm-recombine-distance/run-test.sh
```

This end-to-end microbenchmark isolates the two communication boundaries
around a two-array analog operation. It places a 512-word activation producer,
two 256x512 MVM arrays, and a two-partial recombination task on a fixed 9x9
mesh. Both arrays are programmed and warmed before the producer is released,
so task timing excludes cold-start matrix installation.

The quick test covers colocated execution, an eight-hop activation route, an
eight-hop partial-result route, and the combined eight-plus-eight-hop case.
The full 35-point `{0,1,2,4,8}` by `{0,1,2,4,8}` placement surface is:

```bash
./tests/producer-mvm-recombine-distance/run-sweep.sh
```

Every trial retains task markers, packet traces, receive-DMA traces, analog
phase traces, router statistics, and a joined `results.csv` under
`build/tests/producer-mvm-recombine-distance/`. The experiment contract and
artifact layout are documented in
[`tests/producer-mvm-recombine-distance/README.md`](../tests/producer-mvm-recombine-distance/README.md).
The initial result is summarized in
[`results/producer-mvm-recombine-distance-2026-07-30.md`](../results/producer-mvm-recombine-distance-2026-07-30.md).

## 3x3 computation pipeline

```bash
./tests/mesh-pipeline/run-test.sh
```

This uses the same nine-tile mesh as the routed communication proof, but each
tile executes a local float32 computation before forwarding the payload. The
application-level successor sequence is:

```text
0 -> 1 -> 2 -> 5 -> 4 -> 3 -> 6 -> 7 -> 8 -> 0
```

Tile 0 dispatches `0.0`. Every tile adds its zero-based tile ID, producing the
sequence `0, 1, 3, 8, 12, 15, 21, 28, 36`. Tiles 1 through 8 validate their
expected input and output bit patterns. Tile 0 accepts the returned result only
if it is exactly float32 `36.0`.

The runner also checks that each serpentine neighbor link carries exactly one
packet and that the final result follows the expected four-hop route from tile
8 back to tile 0. Statistics are written to
`build/tests/mesh-pipeline/router-statistics.csv`.

## Distributed 13-task matrix-vector pipeline

```bash
./tests/distributed-matvec/run-test.sh
```

This is the first test that uses the runtime library across the complete 3x3
mesh. It compiles nine distinct tile ELFs containing 13 globally numbered
4x4 matrix-vector tasks:

```text
tile 0: tasks 0 and 9       tile 5: task 5
tile 1: tasks 1 and 10      tile 6: task 6
tile 2: tasks 2 and 11      tile 7: task 7
tile 3: tasks 3 and 12      tile 8: task 8
tile 4: task 4
```

Every task owns a fixed matrix and consumes and produces a rank-one MLIR-style
memref containing four `float32` values. Execution 0 begins with
`[1,2,3,4]`. Each stage performs all 16 multiply-accumulate terms and sends
the resulting vector to the next globally numbered task. The exact final
vector is `[398,82,120,243]`.

Each vector is exactly four independent 32-bit float transfers. There is no
runtime header, transmitted task ID, transmitted execution ID, length field,
or stop word. The compiled ELF determines which local task receives the next
four words, and each worker exits after its last assigned task. The runner
verifies all 13 task executions, the final value, and Merlin statistics showing
exactly 52 ejected 32-bit words.

The two second-pass mappings can be compared with repeated foreground runs:

```bash
RUNS=20 ./tests/distributed-matvec/compare-mappings.sh
```

The baseline assigns tasks 9-12 to tiles 0-3. The reverse mapping assigns
them to tiles 8-5 and passes task 8's output directly to task 9 in tile 8's
local memory. The comparison validates the final vector on every run, reports
mean and median SST completion times, and verifies the deterministic router
counts. Those completion times combine fd 41 synchronized RISC-V instruction
cycles with modeled Merlin network time. Directional word-hop counts remain
the topology-independent measure of mesh work.

## Optional private-L1 memory backend

```bash
./tests/memory-hierarchy-l1/run-test.sh
```

The runner builds one bare-metal ELF and executes it three times. Native mode
must complete with zero StandardMem requests. Detailed memHierarchy mode
attaches one private 32 KiB, four-way L1 to the same tile and requires every
timed QEMU RAM data access to receive an SST response before the hart resumes.
The third run enables initialization batching and verifies exactly one
`MEMORY_INIT_COMPLETE` handshake, nonzero aggregate byte and cycle counts, and
the return to detailed L1 timing after the guest marker.

The reference detailed run produces 102 timed data accesses: 81 reads and 21
writes. The batched run aggregates its first 55 accesses (144 read bytes and
104 write bytes) into one ten-cycle initialization event, then sends the
remaining 47 accesses through StandardMem. The exact 378-instruction count
remains identical in all three modes because the memory backend changes timing
rather than functional execution. The four-layer test above then validates
the same optional backend with four compiler-generated tile ELFs, four private
L1s, analog computation, and inter-tile tensor routing.

### Controlled private-L1 timing

```bash
./tests/memory-timing-validation/run-test.sh
```

Two purpose-built guests validate the exact 32 KiB, four-way, 64-byte-line L1
contract. A conflict/LRU case maps five lines into one set and checks read and
write hits plus the exact replacement victim. A capacity case fills all 512
lines, refreshes one line, inserts line 513, and checks the resulting eviction.

At the fixed 1 GHz configuration, each hit must take five cycles and each miss
61 cycles. The conflict case observes four hits and six misses for 386 cycles;
the capacity case observes one hit and 514 misses for 31,359 cycles. The
analyzer checks every address, direction, request/response timestamp, cache
statistic, and total wait cycle.

## Mittens element tests

```bash
./components/elements/mittens/tests/run-test.sh
```

The element-local runner first builds and executes the host-side analog device
test. That test verifies five-operation command handling, native C++ float32 MVM,
array-to-array movement, invalid-array status, two eleven-beat 81-word matrix
ingresses, two two-beat nine-word vector ingresses, and configured compute
latency. It also proves that one shared 256-bit link grants at most one beat
per cycle with round-robin arbitration, that independent array computes
overlap, that opposite transfer directions serialize, that `MoveVector`
crosses the link twice, and that every array stream remains ordered. The
bridge test validates independent per-array shared-memory
channels, acceptance, completion, and output payloads.
The CrossSim backend test then creates two independently owned CrossSim
backends, programs different matrices at the same local array ID, and verifies
both float32 MVM results. The SST configuration proof creates two tiles with
three independent CrossSim `AnalogCore` objects each, using the same fixed
`100 x 64` geometry.

The same runner also exercises component registration, managed single-tile
boot, four independently managed tiles, the two-tile NIC bridge, and a live
two-tile performance profile whose report must recover both transferred
32-bit words and both directional word-hops.

All runners remain in the foreground and return a nonzero status on failure.
