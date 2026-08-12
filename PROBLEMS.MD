# Parallel SST Transformer Simulation Problems

## Purpose

This document records why eight SST threads did not complete the 4x4
transformer simulation within ten minutes. The evidence shows that SST
parallel execution works. The workload and test harness prevent useful
parallel scaling.

## Experiment configuration

The experiment used this configuration:

| Parameter | Value |
| --- | --- |
| Model | `transformer_block_with_bias` |
| Mesh | 4x4 |
| Active tiles | 16 |
| Compiler digital workers | 8 |
| SST threads | 8 |
| SST partitioner | `sst.simple` |
| Memory backend | `memhierarchy` |
| L1 organization | One private 32 KiB L1 per tile |
| CPU clock | 1 GHz |
| CPU issue width | 1 |
| Analog arrays per tile | 64 |
| Analog array shape | 1024x512 |
| Memory initialization batching | Disabled |

The compiler created all 16 tile ELFs. SST started all 16 QEMU processes.
SST used approximately 500 percent host CPU during the experiment.

The simulation did not report an SST, QEMU, bridge, or protocol error. It did
not reach a simulated completion time. The experiment stopped after 10 minutes
and 13 seconds.

## Finding 1: The test harness initializes unused buffers

Each active tile allocates eight model input buffers and eight model output
buffers. Each buffer contains 65,536 `float` values.

The startup loop writes every value in every buffer. It does this operation on
all active tiles, without regard to resource ownership.

One tile performs this number of stores:

```text
8 buffer indexes
x 65,536 values
x 2 stores per value
= 1,048,576 stores
```

The complete 16-tile deployment performs this number of stores:

```text
1,048,576 stores per tile
x 16 tiles
= 16,777,216 stores
```

Each tile writes 4 MiB before normal execution. Most tiles do not use most of
these buffers. The output buffers are in `.bss`, so they already contain zero.

The source of these stores is
`tests/sculptor-ra-tree-model-suite/tile-main.cpp`.

## Finding 2: Each startup store uses a memory handshake

The memHierarchy backend uses detailed memory timing. The current configuration
sets `memory_init_batching` to `false`.

QEMU sends each timed startup access to SST. QEMU then waits for the
corresponding memory response. This sequence adds host synchronization to each
of the 16,777,216 startup stores.

The simulator already contains an aggregate initialization mode. This mode
records the byte count and charges a configured bandwidth and setup latency.
The model suite does not enable this mode.

## Finding 3: Tile 0 owns most of the transformer work

Tile 0 contains 16,787,744 bytes of read-only constants. Each other tile
contains between 1,432 and 10,008 bytes of read-only constants.

Tile 0 owns all eight analog matrix setup tasks. It also owns 32 compute tasks.
Thus, tile 0 owns 40 of the 113 total tasks.

The other tiles own between two and seven tasks each. The compiler distributes
some digital tasks, but it does not distribute the analog matrices in this
configuration.

The `arrays-per-core=64` configuration lets one tile contain all eight arrays.
As a result, the compiler has no capacity constraint that forces analog
placement across the mesh.

## Finding 4: SST balances components instead of measured work

The `sst.simple` partitioner assigns two tile groups to each SST thread. It
assigns a weight of `1` to each component.

The resulting task distribution is:

| SST thread | Assigned tiles | Combined tasks |
| ---: | --- | ---: |
| 0 | 1, 3 | 11 |
| 1 | 7, 14 | 11 |
| 2 | 5, 8 | 11 |
| 3 | 2, 15 | 8 |
| 4 | 11, 13 | 11 |
| 5 | 4, 9 | 9 |
| 6 | 0, 6 | 45 |
| 7 | 10, 12 | 7 |

Thread 6 owns tile 0. It receives four to six times more tasks than most other
threads. It also receives almost all constant data and every analog setup task.

The partitioner cannot detect this difference from the current component
weights. The partition file is in the corresponding transformer build
directory as `partition.txt`.

## Finding 5: The transformer graph limits independent execution

Eight compiler digital workers do not create eight independent transformer
executions. They place routines on different tiles.

Each routine still waits for its input resources and dependencies. A downstream
tile cannot continue until its producer sends the required activation.

SST must preserve these causal relationships across worker threads. Faster
workers eventually wait for the thread that owns tile 0. Therefore, the slowest
critical path controls the completion time.

## Root cause

SST parallel execution is active. The observed 500 percent CPU use proves that
multiple host cores process simulation events.

The parallel execution cannot divide the long serial work inside tile 0. It
also cannot remove the millions of startup memory handshakes.

The total wall time follows this approximate relationship:

```text
parallel wall time
  = longest SST worker path
  + cross-thread synchronization
  + QEMU/SST memory handshakes
```

Thread 6 has the longest worker path. The task graph requires data from that
path, so the other workers wait.

## Required corrective work

### 1. Correct the test harness

Initialize only the model buffers that a tile owns. Use the resource byte size
from the tile ABI for each buffer.

Do not write zero to complete output buffers during startup. Use the existing
zero state of `.bss`.

Do not allocate eight fixed 65,536-value buffers for every tile. Allocate or
reserve storage from the actual deployment requirements.

### 2. Use aggregate initialization timing

Expose `memory_init_batching` as a model-suite parameter. Enable it for the
initialization experiment.

Keep the configured byte count, transfer width, and setup latency in the SST
timing model. Document that this mode does not model each startup L1 access.

### 3. Distribute analog matrix ownership

Run an experiment with `arrays-per-core=1`. This constraint forces the eight
transformer arrays onto multiple tiles.

Alternatively, add a compiler placement cost for matrix bytes, setup time, and
analog task count. Do not use task count as the only placement cost.

### 4. Add workload-aware SST weights

Give each tile group an estimated host-work weight. Include these quantities in
the weight:

- The number of tasks.
- The number of timed memory operations.
- The number of constant bytes.
- The number of route operations.
- The number of analog setup and compute operations.

This correction improves host wall time only. It must not change simulated
hardware time.

### 5. Repeat the scaling experiment

Run the corrected transformer with one SST thread. Then run the same deployment
with two, four, and eight SST threads.

Use the same ELFs for all thread-count experiments. This requirement removes
compiler placement as an uncontrolled variable.

## Acceptance criteria

The corrected experiment must meet all these criteria:

1. All 16 active tiles report `SCULPTOR_RA_SIM_PASS`.
2. All thread counts produce the same guest output.
3. All thread counts produce the same simulated completion time.
4. All thread counts produce the same task and packet totals.
5. The eight-thread run uses more than one host core.
6. The result file records the SST thread count and wall time.
7. The startup profile separates initialization from model execution.

## Conclusion

The current result is not evidence that SST parallel execution fails. It is
evidence that the deployment has a large serial critical path.

The fixed test buffers create unnecessary memory traffic. Disabled initialization
batching converts that traffic into millions of host handshakes. Compiler
placement then puts the dominant transformer work on tile 0.

These three effects hide the possible speedup from parallel SST execution.
Correct them before the next transformer scaling measurement.
