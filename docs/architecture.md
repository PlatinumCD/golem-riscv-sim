# System architecture

Golem models a two-dimensional mesh of programmable tiles. Each tile has a
RISC-V hart with RVV support, configurable local memory, and network DMA.
Analog matrix-compute arrays are optional.

The architecture is parameterized. A simulation selects tile count, memory
capacity, clocks, link widths, and DMA concurrency. Those settings must
accompany performance results; a particular study is not the machine specification.

Each section pairs a component description with its configuration. **Defaults
are source defaults, not a prescribed deployment.** Builder arguments configure
the connected system; tile, router, NIC and controller parameters configure
their respective SST components. Those namespaces are not interchangeable.

| Component | Scope | Responsibility |
|---|---|---|
| [Mesh](#mesh-network) | Deployment | Tile locations, links, width and propagation delay |
| [Router](#router) | One per mesh location | Routing, arbitration, buffering and backpressure |
| [NIC](#network-interface-nic) | One per active tile | Local injection/ejection and packet queues |
| [CPU and RVV](#cpu-and-rvv) | One hart per active tile | Guest execution and instruction issue |
| [SPM](#scratchpad) | Private to each enabled tile | Local storage and bank/port service |
| [TX/RX DMA](#tx-and-rx-dma) | Tile-local lanes | Transfers between memory and the NIC |
| [Global RAM](#shared-global-ram-and-dma) | Deployment | Explicit shared-memory service and readiness |
| [Analog arrays](#analog-accelerator-path) | Optional, per tile | Matrix computation and analog data transfers |
| [Synchronization](#synchronization) | Sideband controllers | Initialization and epoch release |
| [Simulation runtime](#functional-execution-and-timing) | Host | Functional state, timed execution and evidence |

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

## Mesh network

The mesh is the arrangement of routers and their neighbor links. Boundary
routers have fewer neighbors. Locations without active guest tiles can still
forward traffic through their routers.

Configure the system with these **[`build_mesh()` arguments](../tests/support/mesh.py)**:

| Argument | Default | Meaning |
|---|---|---|
| `width`, `height` | Required | Columns and rows; addressable locations = width × height |
| `active_tiles` | All locations | Tile IDs that run guest programs |
| `images`, `qemu_path` | Required | Guest images and QEMU executable |
| `mesh_router_backend` | `"merlin"` | `"mittens"` selects the Golem wormhole router/NIC below |
| `mesh_link_width_bits` | `32` | Width of each directed physical link; positive multiple of 32 bits |
| `mesh_link_clock` | `"1GHz"` | Router/NIC transfer clock |
| `mesh_link_latency` | `"10ns"` | SST link delay on cardinal and local NIC–router connections |
| `network_cell_words` | `1` | Number of 32-bit words per configured network cell |
| `network_buffer_cells` | `16` | Endpoint buffer sizing in cells |
| `network_packet_words` | Derived; `16` with defaults | Maximum request words; must fit selected endpoint/router/NIC buffers |
| `tile_params` | `{}` | Tile settings: CPU, SPM, DMA, analog and diagnostics |

Select **`mesh_router_backend="mittens"` explicitly** for the router behavior
described here. The shared builder defaults to Merlin, a separate supported
backend. Link propagation delay is distinct from router pipeline latency.

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

## Router

Each `mittens.wormholeRouter` forwards packets between neighboring routers
and its local NIC. Inputs have finite buffers and credits. Eligible packet
heads use round-robin output arbitration; an output is reserved through the
packet tail. A full downstream buffer can therefore block upstream traffic.

These are **router component parameters**, not tile parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `id`, `mesh_width`, `mesh_height` | Required | Router location and mesh geometry |
| `clock` | `"1GHz"` | Router and physical-link transfer clock |
| `link_width_bits` | `32` | Physical output width per cycle |
| `input_buffer_flits` | `32` | Capacity per input port, in 32-bit flits |
| `pipeline_cycles` | `3` | Head-flit route/switch pipeline latency, in router cycles |
| `tx_streams`, `rx_streams` | `1`, `1` | Local injection/ejection lanes; independently 1, 2 or 4 |
| `packet_burst_coalescing` | `false` | Optional coalesced event mode; multi-RX requires flit-level mode |
| `route_overrides` | Empty | Experimental destination:output pairs; E=0, W=1, S=2, N=3 |
| `loopback_injection_output` | `-1` | Experimental initial direction for self-addressed traffic; disabled by default |

Builder arguments `wormhole_input_buffer_flits` and `wormhole_pipeline_cycles`
map to `input_buffer_flits` and `pipeline_cycles`. The builder supplies IDs,
geometry, link settings and local lane counts.

More local lanes do not duplicate cardinal outputs. Four flows requesting
East still share one East link, regardless of `tx_streams`.

Sources: [router parameters](../src/sst/network/wormholeRouter.h),
[configuration/validation](../src/sst/configuration/networkConfiguration.cc).

## Network interface (NIC)

The `mittens.wormholeNIC` joins a tile to its router. It queues outgoing
flits, obeys router credits, receives incoming flits and delivers complete
network requests. Local injection/ejection capacity is separate from cardinal
link capacity.

These are **NIC subcomponent parameters**:

| Parameter | Default | Meaning |
|---|---|---|
| `endpoint_id`, `network_size` | Required | Endpoint identity and addressable count |
| `mesh_width`, `mesh_height` | `0`, `0` | Optional geometry; supplied by the mesh builder |
| `clock`, `link_width_bits` | `"1GHz"`, `32` | Local lane transfer clock and width |
| `tx_streams`, `rx_streams` | `1`, `1` | Local lane counts; independently 1, 2 or 4 |
| `router_buffer_flits` | `32` | Initial credits per TX lane and shared ejection capacity |
| `injection_buffer_flits` | `64` | Outgoing queue capacity per TX lane |
| `packet_burst_coalescing` | `false` | Optional coalesced mode; incompatible with multi-RX |

Builder argument `wormhole_injection_buffer_flits` sets the injection queue.
`wormhole_input_buffer_flits` sets both router input capacity and NIC credit
configuration. The NIC supports one virtual network.

The builder maps tile `tx_dma_streams` / `rx_dma_streams` to router and NIC
`tx_streams` / `rx_streams`, and connects matching local ports. It also sets
tile `network_tail_delivery=true` for Mittens so completion follows tail
arrival; that tile parameter defaults to false when configured directly.

Source: [NIC parameters](../src/sst/network/wormholeNetworkInterface.h).

## CPU and RVV

Each managed tile has one hart. Configure its resources in **`mittens.tile`**
(the mesh builder accepts these through `tile_params`):

| Parameter | Default | Meaning |
|---|---|---|
| `cpu_clock` | `"1GHz"` | CPU issue clock; also the SPM timing clock |
| `cpu_issue_width` | `1` | Scalar front-end issue width: 1, 2 or 4 |
| `riscv_vector_enabled` | `true` | Enable the standard RISC-V V extension |
| `riscv_vector_length_bits` | `256` | VLEN; power of two from 128 through 1024 bits |
| `riscv_vector_element_bits` | `64` | Maximum element width, ELEN; power of two from 8 through 64 bits |
| `memory` | `"16M"` | QEMU control/program RAM, not SPM or deployment-wide RAM |
| `memory_backend` | `"native"` | Ordinary-memory timing: native, memhierarchy or streaming |
| `memory_load_queue_entries` | `8` | Timed ordinary-memory load queue capacity |
| `memory_store_buffer_entries` | `1` | Timed ordinary-memory store-buffer capacity |

When using `build_mesh()`, select ordinary-memory timing with its
`memory_backend` argument. An optional StandardMem/memHierarchy connection
does not make a cache hierarchy intrinsic to the SPM architecture.

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

Configure SPM in **`mittens.tile` / `tile_params`**:

| Parameter | Default | Meaning |
|---|---|---|
| `scratchpad_enabled` | `false` | Enable private SPM |
| `scratchpad_bytes` | `262144` (256 KiB) | Capacity per tile |
| `scratchpad_banks` | `8` | Independently arbitrated banks |
| `scratchpad_read_ports` | `1` | Read ports per bank |
| `scratchpad_write_ports` | `1` | Write ports per bank |
| `scratchpad_access_width_bits` | `256` | Port width; 32 bytes at the default |
| `scratchpad_latency_cycles` | `1` | Access completion latency in CPU cycles |
| `scratchpad_dma_bytes_per_cycle` | `32` | DMA service bandwidth in bytes per CPU cycle |
| `scratchpad_dma_setup_cycles` | `8` | DMA setup cost in CPU cycles |

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

## TX and RX DMA

TX moves submitted data toward the network. RX moves incoming data into a
posted destination range. Configure them in **`mittens.tile` / `tile_params`**:

| Parameter | Default | Meaning |
|---|---|---|
| `tx_dma_streams` | `1` | Independent TX DMA/local injection lanes: 1, 2 or 4 |
| `tx_dma_fifo_bytes` | `128` | Bounded TX FIFO capacity per lane; zero disables timed TX DMA |
| `rx_dma_streams` | `1` | Independent RX DMA/local ejection lanes: 1, 2 or 4 |
| `rx_dma_queue_depth` | `4` | Incoming burst queue depth: 1 through 4; distinct from lane count |
| `rx_dma_streaming` | `false` | Release arrived DMA-owned fragments before the full frame arrives |
| `rx_dma_clock` | `"1GHz"` | Ordinary-memory RX DMA clock |
| `rx_dma_width_bits` | `256` | Ordinary-memory RX width; positive multiple of 32 bits |
| `rx_dma_setup_cycles` | `8` | Ordinary-memory RX setup per descriptor, in RX DMA cycles |

**SPM-backed RX uses `scratchpad_dma_*` and CPU-cycle timing**, not the
separate ordinary-memory RX clock/rate. SPM-backed TX also consumes common
SPM service. Use the Mittens router/NIC for matching multi-lane connections.

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

## Shared global RAM and DMA

The `mittens.globalRAMController` provides an explicit DMA path through
dedicated SST links, **not the mesh**. It models service channels, request
queues and data readiness. Transfers also consume the participating tile's
SPM service. This shared RAM is not a shared L2 cache.

These are **global-RAM controller parameters**:

| Parameter | Default | Meaning |
|---|---|---|
| `capacity_bytes` | `34359738368` (32 GiB) | Deployment-wide capacity; tile `global_ram_bytes` must agree |
| `tile_count`, `active_tiles` | `1`, all IDs | Addressable and participating tiles |
| `clock` | `"1GHz"` | Controller arbitration/service clock |
| `channels` | `1` | Independent RAM service channels |
| `queue_depth` | `16` | Total request queue capacity |
| `per_tile_queue_depth` | `8` | Request queue capacity per tile |
| `setup_cycles` | `8` | Setup cost per physical request |
| `bytes_per_cycle` | `32` | Transfer bandwidth per channel |
| `burst_bytes` | `64` | RAM burst size |
| `fixed_latency_cycles` | `2` | Additional latency per burst |
| `maximum_request_bytes` | `4294967295` | Maximum physical DMA request size |
| `dependency_mode` | `"bulk_barrier"` | Readiness policy: bulk_barrier or exact_dependencies |
| `demand_write_burst` | `0` | Bounded priority for writes needed by blocked reads; zero disables |
| `read_priority_burst` | `0` | Bounded read priority before forcing a ready write; zero disables |
| `reserved_read_channels` | `0` | Channels withheld from writes; must leave at least one write channel |

Per-request service is `setup_cycles + ceil(bytes / burst_bytes) ×
fixed_latency_cycles + ceil(bytes / bytes_per_cycle)`, in controller cycles.
Readiness and queueing delays are separate from service.

Source: [global-RAM configuration](../src/sst/configuration/globalRAMConfiguration.cc).

## Analog accelerator path

Configure optional arrays in **`mittens.tile` / `tile_params`**:

| Parameter | Default | Meaning |
|---|---|---|
| `analog_array_count` | `0` | Arrays instantiated per tile; zero disables |
| `analog_array_rows` | `100` | Physical rows per array |
| `analog_array_columns` | `100` | Physical columns per array |
| `analog_link_clock` | `"1GHz"` | Shared analog transfer clock |
| `analog_compute_latency_cycles` | `100` | Compute latency in analog-link cycles |
| `analog_backend` | `"native"` | Numerical backend: native or crosssim |
| `crosssim_config` | Empty | Optional CrossSim JSON configuration |

Array count and geometry follow the deployment-wide uniform configuration
contract. The shared analog link width and four-command queue depth described
below are implementation constants, not additional exposed parameters.

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

## Synchronization

Initialization and epoch barriers are modeled sideband controllers. Their
events are not NoC payload bytes. Programs must distinguish local completion,
transfer completion, and barrier release. A study using sideband barriers
does not measure the cost of implementing the same coordination with mesh
messages.

| Owner / parameter | Default | Meaning |
|---|---|---|
| Tile: `memory_init_barrier_tiles` | `0` | Initialization participants; zero disables |
| Tile: `epoch_barrier_epochs` | `0` | Epoch count including boot epoch zero; zero disables |
| Both controllers: `tile_count`, `active_tiles` | `1`, all IDs | Addressable and participating tiles |
| Both controllers: `clock` | `"1GHz"` | Controller clock |
| Both controllers: `release_cycles` | `1` | Delay after the last arrival, in controller cycles |
| Initialization: `release_tiles_per_cycle` | `1` | Maximum tile releases per controller cycle |
| Epoch: `epoch_count` | `1` | Number of accepted epochs |

The builder's `initialization_barrier` and `epoch_barrier` mappings configure
these controllers and wire deployment membership.

Sources: [initialization controller](../src/sst/synchronization/memoryInitializationBarrierController.h),
[epoch controller](../src/sst/synchronization/epochBarrierController.h).

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

## Configuration and measurement

The following **tile parameters** control host execution and diagnostics, not
extra hardware capacity:

| Parameter | Default | Meaning |
|---|---|---|
| `launch_mode`, `elf`, `qemu_path` | `"disabled"`, empty, `"qemu-system-riscv64"` | Guest launch; builder supplies managed mode, image and executable |
| `sync_instruction_quantum` | `1000` | Maximum instructions per host grant, not CPU issue width |
| `qemu_ready_set_workers` | `1` | Host capture workers, not simulated cores |
| `profile_mode` | `"off"` | off, summary or trace |
| `profile_output_directory` | Empty | Tile performance output location |
| `task_trace_directory`, `serial_output_directory` | Empty | Task traces and guest UART output |
| `progress_snapshot_interval_ms` | `10000` | Host wall-time snapshot interval |
| `progress_watchdog_ms` | `60000` | Host wall-time progress watchdog; zero disables |

The [complete tile reference](parameters.md) includes additional replay,
batching and ordinary-memory controls. Router `flit_trace_path` /
`packet_trace_path` and NIC `receive_flit_trace_path` enable network traces;
all default to empty paths.

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
