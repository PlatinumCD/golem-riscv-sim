# Timing model

SST owns simulated time; QEMU executes each tile's RISC-V instructions and
functional memory/device operations. Timing comes from CPU issue accounting
and explicit memory, network, DMA, and analog controllers.

## CPU and bridges

The [CPU ledger](../src/sst/execution/cpuExecutionLedger.h) charges each issue
region:

```text
guest_cpu_cycles = retired instructions  # scalar + vector, one issue per cycle
```

Component defaults are a 1 GHz CPU, issue width 1, and a 1,000-instruction
grant. The managed core is single-issue. Device and memory waits add elapsed
time without retiring instructions.
[Execution control](../src/sst/execution/cpuExecutionController.cc) schedules
stops and replays batched records using their instruction counts.

Grants limit how much QEMU can execute before reporting back. Without rollback, asynchronous
arrivals during a grant can change the software path observed at its next
boundary. Record the instruction quantum with results. Control handshakes
add no synthetic CPU cycle. RVV issue is counted, but arithmetic dependencies,
operation latency, and VL/SEW/LMUL occupancy are not modeled.

The [launcher](../src/sst/execution/qemuProcess.cc) installs these descriptors.
Protocol versions come from the linked headers:

| FD | Purpose | Protocol version |
|---:|---|---:|
| 41 | Grants, stops, waits, memory records, and completion control | [27](../src/bridge/include/mittens/SyncTileBridge.h) |
| 42 | NIC packet and DMA payload transport | [7](../src/bridge/include/mittens/NICTileBridge.h) |
| 43 | Analog commands, operands, and results | [1](../src/bridge/include/mittens/AnalogTileBridge.h) |
| 44 | Shared sparse global-RAM backing when configured | File backing, not a bridge protocol |

NIC, analog, memory, task, and guest-exit boundaries use fd 41 for timing.
Data in fd 42 or 43 does not independently grant execution. Blocking waits
resume when their controller satisfies the boundary.

## Memory and scratchpad

Every managed tile executes from scratchpad.
Code, constants, data and stack occupy SPM; shared main memory uses explicit DMA.
An 8 KiB instruction cache times every dynamic
fetch and fills from the same SPM banks as data/DMA clients. A one-cycle hit
overlaps the instruction's issue cost; additional lookup and fill time stalls
the core. Instructions that fault retain their fetch time without being
counted as retired instructions. This path uses single-issue, unbatched
execution, with no extra program-memory mapping or data cache. TLB timing is
not modeled. See the
[boot and cache configuration](architecture.md#scratchpad-backed-instruction-cache).

The scratchpad is private to each tile and enabled by default:

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
Read and write ports have separate availability. Cache fills, CPU accesses and DMA
clients share these reservations; conflicts delay service. Capacity and access
ranges are checked. DMA clients also maintain engine availability.

[Memory access control](../src/sst/memory/memoryAccessController.cc) holds a
CPU scratchpad access until its scheduled deadline. An aligned eight-element
RVV e32,m1 load/store is one 32-byte transaction. This instruction-local
combining does not depend on cross-instruction batching and does not eliminate
any modeled bytes. Partial or faulting accesses retain their actual served bytes.

Before the first instruction, the boot image consumes shared-memory read
service and SPM write service. Both must complete before QEMU gets an execution
grant. `SCRATCHPAD_BOOT` reports this interval separately from guest CPU cycles.
Loading bytes into SPM does not prewarm the instruction cache.

The CPU cannot access a separate RAM or data-cache path. Unsupported execution
settings are rejected before launch. Capacity, bank geometry, cache geometry,
clocks and DMA resources remain parameterized.

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
additional. In exact-dependency mode, application reads wait for committed
coverage of their ranges. Loader reads are separately identified and do not
wait for application producers; they still pay shared-memory and SPM service. Configurable
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
uses this router and NIC exclusively. A request is delivered after its tail
arrives; RX must not charge network serialization again. Record widths,
clocks, link latencies, buffer sizes, and lane counts with results.

[TX DMA](../src/sst/network/tx/txController.cc) reserves SPM reads and stages
data in a FIFO before injection. The default FIFO is 128 bytes per lane and
must hold at least one DMA beat. [RX DMA](../src/sst/network/rx/rxController.cc)
reserves SPM writes and schedules completion before authorizing guest visibility.
TX and RX each support 1, 2, or 4 lanes; both default to 1. Multiple lanes still
contend for shared SPM ports and router outputs. Optional RX streaming releases
eligible arrived fragments before the whole frame completes.

SPM-backed RX service is measured in CPU cycles, not `rx_dma_clock` cycles.
The receive burst queue defaults to four entries. TX/RX waits use fd 41 and
controller events. Submitting work asynchronously does not by itself prove
overlap: overlapping service must be visible in the timed trace.

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
[SPM/I-cache regressions](../tests/platform/scratchpad-icache/) check guest
execution, fetches, vector transaction widths and quantum independence.
The [test guide](../tests/README.md) lists the hardware regressions. Use the
[profile analyzer](../tools/analysis/analyze-performance-profile.py) for reports.
