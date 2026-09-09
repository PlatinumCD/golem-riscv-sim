# Timing model

SST owns simulated time; QEMU executes each tile's RISC-V instructions and
functional memory/device operations. Timing comes from CPU issue accounting
and explicit memory, network, DMA, and analog controllers.

## CPU and bridges

The [CPU ledger](../src/sst/execution/cpuExecutionLedger.h) charges each issue
region:

```text
cycles = max(ceil(retired instructions / cpu_issue_width),
             retired vector instructions)
```

Component defaults are a 1 GHz CPU, issue width 1, and a 1,000-instruction
grant. Supported widths are 1, 2, and 4. Occupancy carries across ordinary
quantum ends; architectural device boundaries close the region.
[Execution control](../src/sst/execution/cpuExecutionController.cc) schedules
stops and replays batched records using their instruction counts.

Grants provide bounded functional lookahead. Without rollback, asynchronous
arrivals during a grant can change the software path observed at its next
boundary. Record quantum and batching settings with results. Control handshakes
add no synthetic CPU cycle. RVV issue is counted, but arithmetic dependencies,
operation latency, and VL/SEW/LMUL occupancy are not modeled.

The [launcher](../src/sst/execution/qemuProcess.cc) installs these descriptors.
Protocol versions come from the linked headers:

| FD | Purpose | Protocol version |
|---:|---|---:|
| 41 | Grants, stops, waits, memory records, and completion control | [26](../src/bridge/include/mittens/SyncTileBridge.h) |
| 42 | NIC packet and DMA payload transport | [7](../src/bridge/include/mittens/NICTileBridge.h) |
| 43 | Analog commands, operands, and results | [1](../src/bridge/include/mittens/AnalogTileBridge.h) |
| 44 | Shared sparse global-RAM backing when configured | File backing, not a bridge protocol |

NIC, analog, memory, task, and guest-exit boundaries use fd 41 for timing.
Data in fd 42 or 43 does not independently grant execution. Blocking waits
resume when their controller satisfies the boundary.

## Memory and scratchpad

[Tile configuration](../src/sst/configuration/tileParameters.h) supports
`native`, `memhierarchy`, and `streaming` memory backends. Native is the
component default. MemHierarchy uses the `memoryIF` StandardMem interface for
ordinary RAM data-access timing; QEMU supplies functional data. Streaming
deployments use explicit private scratchpad and shared global-RAM DMA.
Instruction fetch and TLB timing are not detailed models.

The private noncoherent scratchpad is implemented and enabled explicitly with
`scratchpad_enabled` (component default false). Defaults when enabled are:

| Resource | Default |
|---|---:|
| Capacity | 256 KiB |
| Banks | 8 |
| Read / write ports per bank | 1 / 1 |
| CPU access width | 256 bits |
| Access latency | 1 CPU cycle |
| Scratchpad DMA bandwidth | 32 bytes per CPU cycle |
| Scratchpad DMA setup | 8 CPU cycles |

[Scratchpad timing](../src/sst/memory/scratchpad/scratchpadTimingModel.cc)
splits requests into beats, selects a bank using
`floor(offset / CPU_port_bytes) % banks`, and reserves an available port.
Read and write ports have separate availability. CPU accesses and local DMA
clients share these reservations; conflicts delay service. Capacity and access
ranges are checked. DMA clients also maintain engine availability.

[Memory access control](../src/sst/memory/memoryAccessController.cc) holds a
CPU scratchpad access until its scheduled deadline. Transport batching and
run compaction replay accesses through this model; compact host records do
not remove modeled memory service. StandardMem loads and stores have bounded
queues (defaults: eight loads, one store). Fences drain older timed stores;
independent request grouping uses instruction identity and dependency metadata.

Optional initialization batching charges setup plus
`ceil(total_bytes / memory_init_bytes_per_cycle)`, with defaults of two
cycles and 32 bytes/cycle. This aggregates initialization traffic without
warming a modeled cache. See
[initialization control](../src/sst/synchronization/initializationBarrierClient.cc)
and tile parameters for phase/barrier settings.

## Shared global RAM

The [global RAM controller](../src/sst/memory/globalRAMController.cc) serves
explicit DMA over dedicated per-tile SST links, separate from the mesh.
Sparse backing defaults to 32 GiB deployment-wide. Its
[configuration](../src/sst/memory/globalRAMController.h) defaults to one
1 GHz channel, 16 total queued requests, and eight per tile.

For B bytes, controller service is:

```text
setup_cycles
  + ceil(B / burst_bytes) * fixed_latency_cycles
  + ceil(B / bytes_per_cycle)
```

Defaults are eight setup cycles, 64-byte bursts, two fixed cycles per burst,
and 32 bytes/cycle per channel. Readiness and channel queue delays are
additional. The controller supports bulk barriers and exact dependencies;
exact reads wait for committed coverage of their ranges. Configurable
scheduling priorities and channel reservations affect contention.

The [tile DMA client](../src/sst/memory/globalDMAClient.cc) tracks tokens,
local SPM service, and global completion. Waits must satisfy both local and
global deadlines. This is a bounded bandwidth/latency model, not a DRAM
command or cache-coherence model.

## Mesh and NIC DMA

The [wormhole router](../src/sst/network/wormholeRouter.cc) uses 32-bit flits
and configurable physical `link_width_bits`, a positive multiple of 32.
An output can transmit `link_width_bits / 32` flits per router cycle,
subject to credits and readiness. Payload word size does not fix link width.

[Router defaults](../src/sst/configuration/networkConfiguration.cc) are
32-bit width, 1 GHz clock, 32 input flits, and three head-pipeline cycles.
Routing is X then Y unless an explicit route override applies. Arbitration,
output reservation through the tail, downstream credits, and finite buffers
model contention and backpressure. SST link latency is configured separately.

The [wormhole NIC](../src/sst/network/wormholeNetworkInterface.cc) handles
injection and receive delivery. The [mesh builder](../tests/support/mesh.py)
also supports Merlin, which needs explicit tail-delivery timing because head
arrival alone does not mean the payload is complete. Record backend, widths,
clocks, link latencies, buffer sizes, and lane counts with results.

[TX DMA](../src/sst/network/tx/txController.cc) reserves SPM reads and stages
data in a bounded FIFO before injection. The default FIFO is 128 bytes;
zero disables timed TX DMA. [RX DMA](../src/sst/network/rx/rxController.cc)
reserves SPM writes and schedules completion before authorizing guest visibility.
TX and RX each support 1, 2, or 4 lanes; both default to 1. Multiple lanes still
contend for shared SPM ports and router outputs. Optional RX streaming releases
eligible arrived fragments before the whole frame completes.

Without the SPM timing path, the
[receive DMA engine](../src/sst/network/receiveDMAEngine.cc) uses a configured
clock, width, and setup delay (defaults: 1 GHz, 256 bits, eight cycles).
The receive burst queue defaults to four entries. TX/RX waits use fd 41
and controller events.

## Analog and measurements

The [analog device](../src/sst/analog/analogDevice.cc) schedules array commands
and arbitrates one shared half-duplex 256-bit link across all arrays.
Each beat carries up to eight 32-bit words. Independent array computation can
overlap; transfers contend for the link. Compute latency defaults to 100
analog-link cycles with a 1 GHz link clock. Array count defaults to zero
(disabled). Functional backend selection does not create a physical analog
circuit timing model.

[Profiling](../src/sst/profiling/) records CPU issue, device waits, SPM
service/conflicts, DMA, and network activity. Overlapping controller service
counters are not automatically additive wall time. Preserve clock domains
and units when interpreting traces.

[Source tests](../src/sst/tests/) cover the CPU ledger, scratchpad timing,
global-RAM readiness, and wormhole network.
[CPU timing](../tests/platform/cpu-timing/) and
[RVV execution](../tests/platform/riscv-vector/) provide guest-level checks.
Use [analysis tools](../tools/analysis/) for profile/report processing and
[compiler checks](../tools/compiler/) for deployment validation.
