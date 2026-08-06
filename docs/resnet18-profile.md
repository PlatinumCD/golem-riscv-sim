# ResNet-18 8x8 performance profile

This profile explains the first complete ResNet-18 deployment on Platform
v0.1. It is a diagnosis of the current simulator and runtime, not a prediction
of physical accelerator performance.

> Historical timing note: these measurements predate the tile-wide shared
> analog-link model. They were collected when every array had an independent
> 256-bit link. They also use the superseded 4,096-word network timing cell,
> which overcharges partial packets and multi-hop transport. They must not be
> treated as current Platform v0.1 timing.

## Configuration

- 8x8 mesh with 64 QEMU tiles and 19 compiler-scheduled active tiles.
- One bare-metal RISC-V ELF per active tile.
- Four independent 1024x512 analog arrays per tile.
- Native analog backend.
- 1 GHz CPU and analog-link clocks.
- Eight analog-link cycles of configured compute latency.
- Default 32-bit, 1 GHz (4 GB/s) mesh links, represented by 16 KiB timing
  cells.
- One 256-bit, 1 GHz receive DMA channel per tile, with eight setup cycles
  per descriptor and four queued receive bursts.
- 4096 32-bit words per full timing cell.
- 1,000,000-instruction QEMU/SST synchronization quantum.
- `-O3` and full LTO for the tile runtime and generated objects.

The polling, event-driven receive, and receive-DMA runtimes all completed
correctly:

| Uninstrumented run | Polling baseline | Blocking receive | Receive DMA |
| --- | ---: | ---: | ---: |
| Active tiles passed | 19/19 | 19/19 | 19/19 |
| Top-1 class | 620 | 620 | 620 |
| Simulated completion | 2.092070633 s | 1.961999594 s | 1.278562198 s |

Blocking receive reduced modeled completion by 0.130071039 s, or 6.217335%.
Receive DMA removed another 0.683437396 s, or 34.833718%, from that baseline.
It is 38.885324% faster than the original polling result. The generated task
graph, network traffic, analog configuration, and numerical output remained
unchanged. Host wall time is informational and not part of the simulated
result; the final timed receive-DMA run took 58.52 seconds.

## What the result means

The 1.278562198 seconds are real under the single-issue Platform v0.1
configuration used for this profile. They are not yet hardware-meaningful
because the CPU side uses a coarse issue-throughput model and has no cache,
complete memory hierarchy, pipeline-dependency model, or operation-specific
vector latency.

The one-million-instruction quantum used here is a coarse temporal-lookahead
and host-throughput setting, not a pure host-only optimization. An empty
receiver no longer retires that many polling instructions once it has
published `RX_WAIT`.
`RX_WAIT` yields at its exact instruction boundary, and SST resumes the hart
when a Merlin delivery or receive-DMA completion becomes visible. The
destination CPU still parses each five-word frame header, but QEMU moves
delivered payload bursts directly into the registered tensor range. The
remaining CPU cost includes generated task bodies, framing and descriptor
work, and software retries under transmit backpressure.

The measurements cannot be combined into one additive end-to-end pie chart:
tiles, links, and analog arrays run concurrently. Aggregate tile cycles and
aggregate device cycles describe work, while SST completion time describes
the critical end-to-end schedule.

```text
SST completion: 1.278562198 s
|
+-- generated RISC-V task bodies
|   `-- concentrated on early cores, especially cores 0-2
|
+-- deployment runtime
|   +-- blocks on RX_WAIT when only incoming data can make progress
|   +-- parses route headers and registers receive-DMA destinations
|   `-- retries when the transmit path is backpressured
|
+-- analog devices
|   `-- milliseconds of aggregate active work, overlapping by array/tile
|
`-- Merlin mesh
    `-- millisecond-scale busiest-link service, not a two-second bottleneck
```

## Network profile

The successful run moved:

| Measurement | Value |
| --- | ---: |
| Useful inter-tile data | 79,983,388 bytes |
| Useful inter-tile bits | 639,867,104 bits |
| Merlin timing cells | 5,035 |
| Cell-hops across mesh links | 5,665 |
| Mean delivered-cell latency | 3.875091 us |
| Maximum delivered-cell latency | 7.414781 us |
| Busiest directional link | 520 cells |
| Busiest-link serialization time | 2.129920 ms |

The compiler's placement-aware graph reports 153,993,136 total data bytes,
including data relationships that do not all become inter-tile traffic. The
79,983,388-byte figure above is the traffic actually injected by the
deployment.

Injection and router stall counters are useful for locating contention, but
their sums are accumulated across parallel endpoints and ports. They must not
be added to the 1.961999594-second critical path.

### Controlled mesh-width isolation

The same 19 ELFs were run at a fixed 1 GHz mesh clock and
1,000,000-instruction synchronization quantum while only the physical mesh
link width changed. The architectural NIC word remained 32 bits in every run.

| Physical mesh width | Simulated completion | Change from 32-bit | Host wall time |
| ---: | ---: | ---: | ---: |
| 32 bits | 1.961999594 s | baseline | 64.61 s |
| 64 bits | 1.961493738 s | -0.505856 ms (-0.0258%) | 59.35 s |
| 128 bits | 1.962243306 s | +0.243712 ms (+0.0124%) | 57.54 s |
| 256 bits | 1.961118954 s | -0.880640 ms (-0.0449%) | 63.11 s |

All four runs passed 19/19 tiles and produced top-1 class 620. Each run moved
exactly 19,995,847 architectural 32-bit words in each direction at the tile
interfaces, and each submitted exactly 157,220 analog operations. Widening
the physical mesh by eight times therefore changed end-to-end time by less
than one millisecond. The small non-monotonic variation comes from
guest/runtime scheduling and backpressure phase changes at synchronization
boundaries; it is not a bandwidth trend.

This experiment rules out the 32-bit mesh links as the source of the roughly
two-second result. Wider links can reduce network service time, but that
service is already a small and substantially overlapped part of the current
critical path.

## Analog profile

Across the 19 active tiles, the wrapper observed:

| Operation or transfer | Count |
| --- | ---: |
| Matrix setup | 74 operations |
| Vector load | 52,382 operations |
| Compute | 52,382 operations |
| Vector store | 52,382 operations |
| Array-to-array move | 0 operations |
| Input words | 65,616,896 |
| Output words | 53,639,168 |

The analog devices accumulated 14,832,928 active tile-cycles, or 14.833 ms at
1 GHz. The busiest tile accumulated 4.439 ms. Input and output movement
represents about 14.907 million 256-bit link beats before accounting for
overlap between the four arrays. The configured compute contribution is only
`52,382 * 8 = 419,056` aggregate array-cycles.

This model therefore says analog link movement dominates analog compute, but
the analog subsystem still does not explain a two-second completion time.

## Cycle-level runtime sample before blocking receive

An opt-in cycle profiler measured every call to the deployment runtime. It
increased completion time from 2.092070633 s to 2.618090538 s, a 25.1%
perturbation. The following figures are therefore attribution samples rather
than replacement timing results.

The profile accumulated 36,444,193,364 runtime cycles across all 19 active
tiles:

| Runtime activity | Aggregate tile cycles | Share |
| --- | ---: | ---: |
| Ready-queue/idle scans | 27,892,137,404 | 76.53% |
| Generated task execution | 762,455,945 | 2.09% |
| Receive processing | 1,423,297,869 | 3.91% |
| Successful transmit processing | 926,731 | 0.003% |
| Transmit-backpressure retries | 1,235,834,875 | 3.39% |
| Loop/control and profiling overhead | 5,129,540,540 | 14.08% |

These are tile-cycles, not seconds on the critical path. For example, two
tiles polling for one second contribute two billion aggregate tile-cycles
while only one second passes in SST.

The corresponding runtime call counts were:

| Runtime action | Calls |
| --- | ---: |
| Execute a ready task | 123 |
| Idle scan | 175,811,240 |
| Receive one 32-bit word | 19,995,847 |
| Advance a successful transmission | 5,101 |
| Retry a blocked transmission | 7,302,949 |

No active tile reported a blocking NIC-receive stop. Waiting tiles continue
to retire polling instructions until a normal synchronization boundary.

The generated task bodies also contain real work. The largest measured
execution totals were:

| Core | Generated-task cycles |
| ---: | ---: |
| 0 | 220,979,126 |
| 1 | 154,370,821 |
| 2 | 140,121,103 |
| 3 | 54,299,394 |
| 4 | 47,037,120 |
| 7 | 27,345,051 |
| 15 | 26,245,935 |

Cores 0-2 account for 515,471,050 cycles, or 67.6% of measured generated-task
work. Per-task timestamps are needed before attributing those cycles to exact
compiler-generated functions, but the concentration is consistent with the
large early convolution preprocessing routines.

The final core illustrates the waiting cost. Core 18 spent only 243,300
profiled cycles executing its two tasks, but 2,026,852,124 cycles in idle
scans while upstream work completed.

## Cycle-level runtime sample after blocking receive

The updated opt-in profile completed in 2.566924565 simulated seconds versus
1.961999594 seconds for the updated uninstrumented run. That 30.8%
perturbation means the cycle profile remains an attribution tool rather than
an official completion measurement.

Across all 19 active tiles, the updated profile recorded:

| Runtime activity | Aggregate tile cycles |
| --- | ---: |
| Generated task execution | 762,455,949 |
| Ready-queue/idle scans | 0 |
| Receive-wait decision | 27,699 |
| Receive processing | 1,423,524,182 |
| Successful transmit processing | 926,881 |
| Transmit-backpressure retries | 1,196,134,862 |
| Loop/control and profiling overhead | 706,677,736 |

The corresponding runtime action counts were:

| Runtime action | Calls |
| --- | ---: |
| Execute a ready task | 123 |
| Idle scan | 0 |
| Enter receive wait | 137 |
| Receive one 32-bit word | 19,995,847 |
| Advance a successful transmission | 5,101 |
| Retry a blocked transmission | 7,067,959 |

The old profile accumulated 175,811,240 idle scans and 27,892,137,404 idle
cycles. Both are now zero. Waiting work was removed from the guest instruction
stream rather than relabeled as another polling loop.

In the uninstrumented run, the 19 active tiles collectively reported 1,047
`NIC_RECEIVE_WAIT` fd-41 stops. That count need not match the 137 profiled
runtime decisions because the `mesh_nic` receive helper can wait while
draining a partially delivered frame, and because profiling changes
instruction timing. Every wait remains tied to an actual fd-41 boundary and
network delivery.

## Compiler estimate versus executed system

The compiler's placement-aware timing metadata estimated:

| Compiler metric | Value |
| --- | ---: |
| Critical path | 29.859097 ms |
| Aggregate network latency | 38.615836 ms |
| Aggregate contention delay | 18.620417 ms |
| Total graph data | 153,993,136 bytes |

The gap between the 29.859 ms abstraction and the 1.961999594 s synchronized
execution is not a single missing latency. Blocking receive removed idle
polling, but the compiler estimate still does not include per-word receive
handling, transmit retries, or the exact instruction count of generated task
bodies.

## Synchronization-quantum isolation

The mesh was then fixed at 32 bits and 1 GHz while the QEMU/SST instruction
quantum was reduced. These runs used exact fd-41 stops for receive waits and
analog submissions in addition to the ordinary quantum stops.

| Instruction quantum | Simulated completion | Active instructions | Ordinary grants | TX-full stops | RX-wait stops | Host wall time |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000,000 | 1.961999594 s | 3,716,788,843 | 3,733 | 1,407 | 1,047 | 64.61 s |
| 250,000 | 1.989188842 s | 3,738,207,935 | 14,967 | 2,295 | 96 | 70.04 s |
| 100,000 | 1.988390122 s | 3,734,207,412 | 37,356 | 4,037 | 96 | 87.56 s |

Every run passed 19/19 tiles, returned top-1 class 620, transferred the same
19,995,847 words, and submitted the same 157,220 analog operations. The
250,000- and 100,000-instruction results differ by only 0.799 ms, so the
fine-quantum result has converged near 1.988 seconds. It did not converge
toward a value remotely close to the compiler's 29.859 ms estimate.

The model is not completely quantum-invariant yet: the fine-quantum result is
about 26.4 ms (1.35%) above the one-million-instruction result. The changing
instruction and TX-full-stop counts identify the remaining source of that
sensitivity. A sender whose NIC path is full still retries in guest software,
so different QEMU/SST interleavings retire different amounts of retry work.
Receive waiting itself is event driven and no longer spins.

The appropriate next synchronization fix is an event-driven TX-space
doorbell: QEMU should stop at the exact failed transmit boundary, and SST
should resume it when the egress path has capacity. Repeating this quantum
sweep after that change should produce nearly invariant simulated completion
and instruction counts. This is a precision fix for the measured 1.35%
sensitivity; it cannot account for the other roughly 1.96 seconds.

## Task-boundary trace

The optional task tracer emits fd-41 markers immediately before and after
every registered task call. SST timestamps those markers in its global
one-picosecond time base. The markers carry `(tile_id, task_id, execution_id)`
and never enter a mesh packet.

The traced ResNet-18 run passed all 19 active tiles and returned top-1 class
620:

| Measurement | Value |
| --- | ---: |
| Untraced reference completion | 1.961999594 s |
| Traced completion reported by SST | 1.962 s |
| Traced host wall time | 61.70 s |
| Completed tasks | 123 |
| Matched trace events | 246 |
| First task start | 0.051003061 s |
| Last task finish | 1.961986014 s |
| First-to-last task span | 1.910982953 s |
| Aggregate task duration | 0.771752650 s |
| Union of task intervals | 0.768986079 s |
| Global intervals with no task executing | 1.141996874 s |

Only 40.240% of the first-to-last span contains a registered task executing
on any tile. The other 59.760% occurs between task calls. Those gaps are not
necessarily hardware idle time: the guest deployment runtime may be framing,
transmitting, receiving, copying, or checking readiness, and tiles may be
waiting for dependencies. The result isolates the next investigation to the
runtime/data-movement boundary rather than the analog task bodies alone.

Task execution is also nearly serialized. Aggregate task duration exceeds
the union by only 2.766571 ms, so overlap between task calls is just 0.358% of
aggregate task time.

The longest task calls were:

| Tile | Task | Duration |
| ---: | ---: | ---: |
| 0 | 2 | 92.023415 ms |
| 0 | 6 | 59.820334 ms |
| 1 | 18 | 59.820334 ms |
| 1 | 10 | 56.040645 ms |
| 2 | 22 | 56.040645 ms |
| 0 | 5 | 53.270110 ms |
| 2 | 35 | 32.524619 ms |
| 3 | 47 | 32.524619 ms |
| 4 | 57 | 29.815058 ms |
| 15 | 104 | 18.066405 ms |

The longest global gaps between task calls were:

| After | Before | Gap |
| --- | --- | ---: |
| tile 2, task 22 | tile 2, task 24 | 113.158056 ms |
| tile 1, task 10 | tile 1, task 12 | 106.621645 ms |
| tile 3, task 47 | tile 4, task 49 | 57.451448 ms |
| tile 2, task 35 | tile 3, task 37 | 57.192838 ms |
| tile 4, task 49 | tile 3, task 48 | 49.025624 ms |
| tile 4, task 57 | tile 4, task 58 | 48.924289 ms |
| tile 3, task 37 | tile 2, task 36 | 48.820086 ms |

The generated artifacts are:

```text
task-trace.csv          globally sorted start/finish events
task-trace-summary.csv  paired tasks sorted by duration
task-trace-gaps.csv     global no-task intervals sorted by duration
task-trace-raw/         collision-free per-tile SST records
```

## Receive-DMA result

The destination-side DMA path was measured with the same ELFs, task graph,
analog configuration, 32-bit mesh, and 4096-word network timing cells:

| Measurement | Blocking receive | Receive DMA | Change |
| --- | ---: | ---: | ---: |
| Simulated completion | 1.961999594 s | 1.278562198 s | -34.833718% |
| Active-tile instructions | 3,716,788,843 | 1,993,538,431 | -46.363958% |
| Network words transmitted | 19,995,847 | 19,995,847 | unchanged |
| Network words received | 19,995,847 | 19,995,847 | unchanged |
| Analog `SetMatrix` operations | 74 | 74 | unchanged |
| Analog load/compute/store operations | 52,382 each | 52,382 each | unchanged |
| Top-1 class | 620 | 620 | unchanged |

The matching network and analog counts prove that payloads still traverse
SST. The removed work is the destination guest's per-word `RX_DATA` loop.
QEMU copies only bursts already delivered by Merlin into the guest tensor
range, then reports a source-and-route completion to the runtime.

The original receive-DMA measurement made the final delivered-burst copy
functional at the network delivery boundary. The timed tile-local engine now
separately charges that transfer:

| Timed receive-DMA measurement | Value |
| --- | ---: |
| Ideal local-copy completion | 1.278558030 s |
| Timed local-copy completion | 1.278562198 s |
| Critical-path increase | 4.168 us (0.000326%) |
| Payload bursts transferred | 4,944 |
| Payload words transferred | 19,995,392 |
| Aggregate RX DMA active cycles | 2,500,152 |
| Host wall time | 58.52 s |

The aggregate count is exactly the payload serialization plus descriptor
setup:

```text
19,995,392 words / 8 words per cycle
    + 91 descriptors * 8 setup cycles
    = 2,500,152 aggregate DMA cycles
```

Most local transfers overlap with CPU, network, analog, or other tiles' DMA
work, so aggregate DMA cycles are not added to the end-to-end critical path.
The matching 19/19 tile passes and top-1 class 620 verify that delaying guest
visibility did not change the result.

## CPU-frequency isolation

The same ELFs were rerun with only `mittens.tile.cpu_clock` changed from
1 GHz to 3 GHz. Mesh links, analog links, and receive DMA remained at 1 GHz:

| Measurement | 1 GHz CPU | 3 GHz CPU | Change |
| --- | ---: | ---: | ---: |
| Simulated completion | 1.278562198 s | 0.431314372388 s | 2.964339x faster |
| Active-tile instructions | 1,993,541,652 | 1,994,545,465 | +0.050353% |
| Network words | 19,995,847 | 19,995,847 | unchanged |
| RX DMA payload words | 19,995,392 | 19,995,392 | unchanged |
| Analog load/compute/store operations | 52,382 each | 52,382 each | unchanged |
| Top-1 class | 620 | 620 | unchanged |

Perfect one-third CPU scaling would produce 0.426187399333 seconds. The
measured run is 5.126973055 ms slower because device clocks did not change and
their work becomes relatively larger. Timing-dependent transmit
backpressure also added 1,003,813 guest instructions, almost entirely on one
tile. This is further evidence that an event-driven transmit-space stop is
required for an instruction stream that is invariant under clock-frequency
experiments.

The receive-DMA trace passed all 19 active tiles and produced 123 matched
task intervals:

| Trace measurement | Per-word receive | Receive DMA | Change |
| --- | ---: | ---: | ---: |
| First-to-last task span | 1.910982953 s | 1.227524956 s | -35.764735% |
| Aggregate task duration | 0.771752650 s | 0.771752650 s | unchanged |
| Union of task intervals | 0.768986079 s | 0.753735206 s | -1.98% |
| Global no-task gaps | 1.141996874 s | 0.473789750 s | -58.512168% |

The task bodies are unchanged; aggregate duration is exactly unchanged.
Receive DMA primarily removes work between tasks. The largest remaining gaps
are 48.807114 ms between tasks 10 and 12 and 48.338291 ms between tasks 22
and 24. Those are now the best route/runtime boundaries to instrument.

## Conclusions and next changes

Receive DMA is now implemented and measured. It removes guest payload MMIO
loads while preserving all SST mesh traffic. The next targets are:

1. Turn full transmit backpressure into a yield/resume event instead of
   millions of software retries, then repeat the synchronization-quantum
   sweep to verify quantum invariance.
2. Add route-send and route-receive-complete markers around the largest
   between-task gaps to separate runtime copying from dependency wait time.
3. Optimize the longest compiler-generated task calls, starting with tasks 2,
   6, 18, 10, 22, and 5.
4. Add scratchpad capacity, banking, and CPU/DMA port contention around the
   timed local DMA boundary before treating all memory behavior as
   hardware-accurate.
5. Repeat the normal and profiled runs after each change and keep
   uninstrumented SST completion as the official comparison.

The measured 34.833718% receive-DMA improvement is smaller than its 46.363958%
instruction reduction because work and waiting overlap across tiles.
Aggregate tile-cycle savings cannot be converted directly into critical-path
time.

## Reproducing the measurements

Run the uninstrumented deployment:

```bash
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

Override the physical mesh width or clock without changing the architectural
32-bit word format:

```bash
MITTENS_RESNET18_MESH_LINK_WIDTH_BITS=64 \
MITTENS_RESNET18_MESH_LINK_CLOCK=1GHz \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

Run the same binaries and device clocks with a different synchronized CPU
frequency:

```bash
MITTENS_RESNET18_CPU_CLOCK=3GHz \
MITTENS_RESNET18_LOG_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/simulation-cpu-3ghz.log" \
MITTENS_RESNET18_STATS_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/router-statistics-cpu-3ghz.csv" \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

This changes only `mittens.tile.cpu_clock`. The mesh, analog link, and receive
DMA remain at their separately configured frequencies.

The ResNet-18 test defaults to the measured 1,000,000-instruction quantum.
Reducing that to 1,000 creates roughly three orders of magnitude more ordinary
SST/QEMU grant handshakes. It is not needed for receive latency because
`RX_WAIT` creates its own exact fd-41 boundary.

Reproduce a finer-quantum isolation point with:

```bash
MITTENS_RESNET18_SYNC_QUANTUM=100000 \
MITTENS_RESNET18_MESH_LINK_WIDTH_BITS=32 \
MITTENS_RESNET18_MESH_LINK_CLOCK=1GHz \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

Enable the cycle-level runtime profile explicitly:

```bash
MITTENS_RESNET18_RUNTIME_PROFILE=1 \
MITTENS_RESNET18_LOG_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/simulation-runtime-profile.log" \
MITTENS_RESNET18_STATS_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/router-statistics-runtime-profile.csv" \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

Enable wrapper-level instruction, network, and analog counters without the
cycle hook:

```bash
MITTENS_RESNET18_VERBOSITY=1 \
MITTENS_RESNET18_LOG_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/simulation-profile.log" \
MITTENS_RESNET18_STATS_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/router-statistics-profile.csv" \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

Enable globally synchronized task tracing:

```bash
MITTENS_RESNET18_TASK_TRACE=1 \
MITTENS_RESNET18_LOG_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/simulation-task-trace.log" \
MITTENS_RESNET18_STATS_PATH="$PWD/build/tests/sculptor-resnet18-8x8/deployment/router-statistics-task-trace.csv" \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```
