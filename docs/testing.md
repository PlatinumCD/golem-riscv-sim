# Testing

Each complete system scenario owns its sources, SST configuration when
needed, and a foreground `run-test.sh`.

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
prints `Golem Platform v0: single tile booted`, and exits successfully.

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
counts. Those completion times include asynchronous host-QEMU polling and are
not RISC-V execution times; directional word-hop counts are the stable measure
of mesh work.

## Mittens element tests

```bash
./components/elements/mittens/tests/run-test.sh
```

The element-local runner exercises component registration, managed single-tile
boot, four independently managed tiles, and the two-tile bridge.

All runners remain in the foreground and return a nonzero status on failure.
