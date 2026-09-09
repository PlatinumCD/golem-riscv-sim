# System architecture

Golem models a two-dimensional mesh of programmable tiles. Each tile has a
RISC-V hart with RVV support, configurable local memory, and network DMA.
Analog matrix-compute arrays are optional.

The architecture is parameterized. A simulation selects tile count, memory
capacity, clocks, link widths, and DMA concurrency. Those settings must
accompany performance results; a particular study is not the machine specification.

## One tile

The principal local-memory and communication path is:

```text
                  RISC-V hart + RVV
                          ↕
                 Private banked SPM
                    ↙           ↖
               TX DMA           RX DMA
                  ↓               ↑
               TX FIFO       Receive queues
                  ↓               ↑
                  Network interface
                          ↕
                      Mesh router
                    ↕   ↕   ↕   ↕
                    N   E   S   W
```

SPM means scratchpad memory: software-managed storage, not a cache.
A buffer is a range of addresses allocated within that storage; allocating
another buffer does not add banks or ports.

Optional global-memory DMA also accesses the scratchpad. Optional analog
arrays have their own command queues and a shared tile-local analog link.

## Functional execution and timing

Each managed tile runs one single-hart QEMU process under an SST
`mittens.tile` component.

| Owner | Responsibility |
|---|---|
| Guest program | Computation, data placement, transfer submission, and synchronization |
| QEMU | RISC-V instruction semantics, functional guest bytes, and device register state |
| SST tile controllers | CPU accounting, memory service, DMA scheduling, and device completion |
| SST network | Routing, finite queues, arbitration, link service, and backpressure |
| Analog backend | Numerical matrix-vector results |
| SST Core | Simulation time and event delivery |

QEMU and SST communicate through per-tile host shared-memory bridges:

| Child descriptor | Contents |
|---|---|
| 41 | Instruction grants, stop events, and resume |
| 42 | Network data and receive authorization |
| 43 | Analog commands, operands, and results |

Only fd 41 controls guest execution. The host bridges are not physical
network links, and their ring capacities do not define hardware bandwidth.
The [bridge headers](../src/bridge/README.md) define their layouts.

SST grants QEMU an instruction budget, receives execution and device events,
and schedules their modeled effects. Guest exit is also reported at an
instruction boundary. Batching and host parallelism reduce simulation overhead;
they must not create additional simulated resource capacity or bypass deadlines.

## CPU and RVV

The scalar issue width is configurable as 1, 2, or 4. Vector issue is limited
to one instruction per CPU cycle. For an instruction-accounting region:

```text
issue cycles = max(ceil(total instructions / scalar issue width),
                   vector instructions)
```

Total instructions include vector instructions. The ledger charges incremental
occupancy and distinguishes architectural boundaries from host grant boundaries;
applying this formula independently to every host batch is not equivalent.

This accounts for issue throughput, not a detailed pipeline. Memory and device
waits contribute additional elapsed time. The model does not implement a
general out-of-order CPU, instruction-cache/TLB timing, or vector-operation
latency that scales with active vector length.

RVV register length and maximum element width are configurable. With a
256-bit register and 32-bit elements, an m1 vector holds eight elements.
The aligned eight-element SPM load/store path preserves a 32-byte transaction
rather than timing it as eight serialized scalar accesses. Other sizes,
alignment, and instruction forms must not be assumed to have identical service.

Sources: [CPU ledger](../src/sst/execution/cpuExecutionLedger.h),
[execution controller](../src/sst/execution/cpuExecutionController.cc).

## Scratchpad

Each enabled scratchpad is private to its tile and noncoherent. Its guest
base address is `0x90000000`; capacity is configurable. Identical addresses
on different tiles refer to different scratchpads.

The parameter defaults, when SPM is enabled, are:

| Property | Default |
|---|---:|
| Capacity | 256 KiB |
| Banks | 8 |
| Read ports per bank | 1 |
| Write ports per bank | 1 |
| Port width | 256 bits |
| Completion latency | 1 CPU cycle |
| DMA transfer rate | 32 bytes per CPU cycle |
| DMA setup | 8 CPU cycles |

SPM is disabled in the bare component defaults; workloads enable it explicitly.
These defaults are not an assertion that every simulation uses them.

Bank selection uses the configured port width:

```text
stripe_bytes = access_width_bits / 8
bank = floor(spm_offset / stripe_bytes) mod bank_count
```

At 32-byte stripes and eight banks, offsets 0, 32, …, 224 select banks 0–7;
offset 256 selects bank 0 again.

CPU accesses, TX DMA, RX DMA, and global-memory DMA use the same SPM timing
model. Different banks can serve independent requests concurrently. A bank's
read and write ports have separate availability, so a read and write can
overlap. Competing reads on one read port serialize.

A service beat occupies its port for one cycle; configured completion latency
is a separate quantity. Eight banks therefore provide backend parallelism,
not a promise that one CPU stream can issue eight requests per cycle. The
CPU replay path serializes SPM requests; independent DMA clients can expose
additional bank concurrency.

Sources: [address contract](../src/bridge/include/mittens/MemoryMap.h),
[SPM timing model](../src/sst/memory/scratchpad/scratchpadTimingModel.cc).

## Tile-to-tile transfers

For an SPM-backed transfer with timed TX enabled:

```text
Source SPM → TX DMA → bounded FIFO → NIC → mesh
                                            ↓
Destination SPM ← RX DMA ← receive buffering
```

1. Software submits a destination and source range. QEMU preserves the
   functional payload and reports the submission boundary.
2. TX DMA requests source-SPM service through the common arbiter. Completed
   beats become available in a bounded FIFO.
3. The NIC injects available data subject to network capacity. A full FIFO
   stops further prefetch; network backpressure therefore reaches TX DMA.
4. The destination receives data subject to finite buffering and posted
   receive descriptors. RX schedules writes through the common SPM arbiter.
5. SST authorizes completion before QEMU exposes the corresponding DMA result
   to guest software.

QEMU's functional payload snapshot is retained. This models resource service
and availability; it is not a beat-by-beat functional SRAM model for a program
that modifies its source while a transfer is in flight.

TX and RX independently support **1, 2, or 4 lanes**, with a default of one.
The mesh builder connects matching local injection/ejection capacity.
More lanes do not duplicate SPM ports, cardinal links, or destination storage.

Receive scheduling preserves per-source timing order. RX queue depth and RX
lane count are different parameters. Ordinary-memory receive DMA has a
separate timing path; the description above applies to SPM destinations.

A guest may wait for data or transmit capacity through MMIO wait operations.
Those operations recheck availability before blocking. Submitted DMA can
continue while the hart executes independent work. Ping-pong buffering keeps
the compute and transmit data ranges separate, but overlap still depends on
SPM and network availability.

Sources: [TX controller](../src/sst/network/tx/txController.cc),
[RX controller](../src/sst/network/rx/rxController.cc),
[guest interface](../src/platform/devices/mesh-nic.h).

## Mesh network

Tiles use row-major identifiers:

```text
tile_id = y * mesh_width + x
```

The Mittens wormhole router normally routes X first, then Y. Explicit route
overrides are available for experiments. Intermediate routers forward traffic
without involving intermediate guest programs.

Links carry 32-bit flits. Physical link width and clock determine the service
budget: a 32-bit link carries 4 bytes per link cycle; a 128-bit link carries
16 bytes. Headers, partial beats, startup, and stalls reduce useful payload rate.

Input buffering and credits are finite. An output is reserved through a
packet's tail; eligible competing packet heads use round-robin arbitration.
A blocked downstream buffer can stall an upstream output.

Three bandwidth limits must be kept distinct:

- **Tile injection/ejection:** what its DMA and local NIC lanes can supply or accept.
- **One directed link:** what that physical output can carry.
- **Fabric aggregate:** concurrent service across different links.

Crossing flows need not contend if they use independent inputs and outputs.
Flows requesting the same output do contend. A single source with one TX lane
cannot be used to establish the aggregate capacity of four outgoing links.

Link delay, router pipeline delay, packet size, and queue capacity are
configuration choices. Do not apply a Merlin regression's latency formula to
the Mittens wormhole router; both backends exist.

Sources: [router](../src/sst/network/wormholeRouter.cc),
[NIC](../src/sst/network/wormholeNetworkInterface.cc),
[mesh builder](../tests/support/mesh.py).

## Other memory and synchronization paths

The QEMU `memory` setting defaults to 16 MiB of control/program RAM. It is
not the scratchpad capacity or total deployment memory.

Ordinary-memory timing supports native, memHierarchy, and streaming modes.
An optional StandardMem connection does not require a private L1; a cache
hierarchy is not intrinsic to the SPM architecture.

The shared global-RAM controller provides a separate explicit DMA path.
Its capacity, channels, queues, and dependency handling are configurable.
A tile can therefore make data available to another through global memory,
not only through direct NoC messages.

Initialization and epoch barriers are modeled sideband controllers. Their
events are not NoC payload bytes. Programs must distinguish local completion,
transfer completion, and barrier release. A study using sideband barriers
does not measure the cost of implementing the same coordination with mesh
messages.

Sources: [global RAM](../src/sst/memory/globalRAMController.cc),
[synchronization controllers](../src/sst/synchronization/).

## Analog accelerator path

A nonzero analog array count enables matrix-compute hardware. The custom
instructions program a matrix, load a vector, start computation, store an
output, or move an output between local arrays.

Each array has an ordered four-command queue and separate compute state.
All arrays on a tile share one bidirectional 256-bit transfer link. Link
arbitration advances one eight-float32 beat per analog-link cycle; independent
array computation can overlap.

`mvm.set` supports a packed array ID and valid row/column shape. Compact
submissions transfer the valid rectangle, which SST expands to the physical
array geometry. The encoding is defined by the instruction helper and
[analog ISA](analog-isa.md), not a plain array-ID-only operand table.

QEMU snapshots inputs and copies completed outputs. The native or CrossSim
backend computes numerical results; SST schedules transfer and compute
latency. Backend host execution time is not simulated analog latency.
A blocking output store waits for its result while previously submitted work
can continue.

The analog link is separate from the NoC. Its 256-bit width neither widens
mesh links nor guarantees equal digital and analog compute throughput.

Sources: [analog device](../src/sst/analog/analogDevice.cc),
[instruction helper](../src/qemu/instructions/golem-analog/golem_analog_helper.c).

## Configuration and measurement

Use [tile parameters](../src/sst/configuration/tileParameters.h) and the
[configuration owners](../src/sst/configuration/README.md) for supported
settings. Record resolved settings with each result.

Report elapsed execution separately from service counts, queueing, and stalls.
Those resource intervals can overlap. Missing counters are not measured zeros.
Packet-transit sums are not an additional elapsed interval to add to runtime.

CPU/SPM, ordinary-memory RX DMA, network, and analog timing may use different
clock domains. Convert through SST time before comparing them. See
[clock semantics](../src/sst/execution/TIMING.md) and the
[measurement contract](../src/sst/profiling/MEASUREMENT_CONTRACT.md).

This model supports architectural comparisons under explicit assumptions.
It does not establish silicon frequency, power, area, or calibrated processor
latency.
