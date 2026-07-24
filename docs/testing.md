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

## Runtime library

```bash
./runtime/tests/run-test.sh
./tests/runtime-library/run-test.sh
```

The first command runs host-side unit tests for tensor validation, task
registry lookup, task-instance allocation, ready-queue wraparound, 32-bit
transport callbacks, and backpressure behavior. The second builds
`libgolem-runtime.a` with the pinned RISC-V compiler, links it into a
freestanding tile ELF, boots that ELF in QEMU, executes a registered scalar
task, and requires `Golem runtime library: PASS`.

Both proofs are available together as `./bootstrap.sh test-runtime`.

## Single tile

```bash
./tests/hello/run-test.sh
```

This builds one bare-metal ELF, boots it directly with the repository QEMU,
prints `Golem Platform v0.1: single tile booted`, and exits successfully.

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

## Mittens element tests

```bash
./components/elements/mittens/tests/run-test.sh
```

The element-local runner first builds and executes the host-side analog device
test. That test verifies five-operation command handling, native C++ float32 MVM,
array-to-array movement, invalid-array status, an eleven-cycle 81-word matrix
ingress, a two-cycle nine-word vector ingress, and configured compute latency.
It also proves that two arrays transfer and compute concurrently, that
`StoreVector` output takes `ceil(rows / 8)` cycles, that opposite directions
overlap on different links, and that one array remains ordered and
half-duplex. The bridge test validates independent per-array shared-memory
channels, acceptance, completion, and output payloads.
The CrossSim backend test then creates two independently owned CrossSim
backends, programs different matrices at the same local array ID, and verifies
both float32 MVM results. The SST configuration proof creates two tiles with
three independent CrossSim `AnalogCore` objects each, using the same fixed
`100 x 64` geometry.

The same runner also exercises component registration, managed single-tile
boot, four independently managed tiles, and the two-tile NIC bridge.

All runners remain in the foreground and return a nonzero status on failure.
