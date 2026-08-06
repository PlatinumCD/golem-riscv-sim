# Mittens

Mittens is the SST element for QEMU-backed, bare-metal RISC-V compute tiles.

The `mittens.tile` component owns the tile configuration and SST lifecycle and
exposes optional `SST::Interfaces::SimpleNetwork` and
`SST::Interfaces::StandardMem` subcomponent slots. When populated, the
component forwards the SST `init`, `setup`, and `finish` phases to those
interfaces.

With `launch_mode=managed`, the component launches one QEMU child process in
`setup()`, keeps the simulation alive until QEMU exits, and controls that
child through the fd 41 synchronization bridge. SST grants precise icount
quanta and receives total and vector retired counts. It schedules
`max(ceil(total / cpu_issue_width), vector)` cycles on `cpu_clock`, preserving
one vector issue per cycle while allowing a scalar issue width of 1, 2, or 4.
The default `launch_mode=disabled` remains useful for element and
configuration tests.
Normal guest completion is also an fd 41 event, so SST schedules and reaps
QEMU at the reported terminal instruction boundary instead of assigning time
to host-side process polling.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `cpu_clock` | SST clock for synchronized instruction cycles | `1GHz` |
| `cpu_issue_width` | Scalar front-end issue width; one of 1, 2, or 4 | `1` |
| `sync_instruction_quantum` | Maximum instructions in one QEMU grant | `1000` |
| `memory_backend` | Data-memory timing backend: `native` or `memhierarchy` | `native` |
| `memory_init_batching` | Aggregate pre-marker memory traffic into one fd 41 initialization event | `false` |
| `memory_init_bytes_per_cycle` | Aggregate initialization transfer rate | `32` |
| `memory_init_latency_cycles` | Fixed setup cost for the aggregate initialization transfer | `2` |
| `rx_dma_clock` | Clock for the tile-local NIC-to-memory receive DMA engine | `1GHz` |
| `rx_dma_width_bits` | Receive DMA width; a positive multiple of 32 bits | `256` |
| `rx_dma_setup_cycles` | Setup cycles charged once per receive descriptor | `8` |
| `rx_dma_queue_depth` | Delivered receive bursts allowed to await DMA service | `4` |
| `mesh_width` | Tile columns used to calculate Manhattan distance | `0` |
| `mesh_height` | Tile rows used to validate the routed topology | `0` |
| `profile_mode` | Performance output mode: `off`, `summary`, or `trace` | `off` |
| `profile_output_directory` | Directory for per-tile profile CSV files | Empty |
| `task_trace_directory` | Optional directory for collision-free per-tile task trace CSV files | Empty |

With `memory_backend=memhierarchy`, the `memoryIF` slot is required. QEMU
publishes each guest RAM data access through fd 41, Mittens sends a matching
StandardMem request, and the hart resumes only after the response. The
functional load or store still uses QEMU RAM; memHierarchy owns timing and
cache state, not guest bytes. Each tile configuration attaches its own private
L1. Native mode creates no StandardMem traffic.

Initialization batching is valid only with `memory_backend=memhierarchy`. While
it is active, QEMU counts guest RAM reads and writes without yielding. The
guest calls `mesh_nic::complete_memory_initialization()` once after boot
registry setup. QEMU then sends one `MEMORY_INIT_COMPLETE` event, and SST
charges:

```text
memory_init_latency_cycles
  + ceil((read_bytes + write_bytes) / memory_init_bytes_per_cycle)
```

The marker does not bypass modeled time or alter functional memory. It replaces
millions of host synchronization round trips with one analytically timed
initialization transfer. All later accesses return to the detailed
StandardMem/private-L1 path.

When `networkIF` is attached, the component creates the fd 42 data bridge for
the custom QEMU `mittens-nic` device. A transmit publishes data on fd 42 and
yields through fd 41. See
[`../../../docs/timing-model.md`](../../../docs/timing-model.md) for modeled
timing and remaining CPU-model limits.

Each tile owns one receive DMA channel. Same-tile payload bursts serialize;
channels belonging to different tiles overlap. SST charges
`rx_dma_setup_cycles` once per descriptor plus
`ceil(words * 32 / rx_dma_width_bits)` cycles per burst. QEMU cannot consume
a timed burst until the SST completion event authorizes it. The finite queue
depth, which must be from one through four, applies endpoint backpressure when
local writes fall behind the mesh.

## Analog device

Setting `analog_array_count` to a nonzero value gives a tile one local
`AnalogDevice`. It supports both the native C++ float32 MVM backend and the
CrossSim backend. The managed QEMU child receives the analog bridge on file
descriptor 43 and submits compiler-emitted Golem instructions through it.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `analog_array_count` | Simulation-wide array count on every tile | `0` |
| `analog_array_rows` | Simulation-wide row count for every array | `100` |
| `analog_array_columns` | Simulation-wide column count for every array | `100` |
| `analog_backend` | Numerical backend: `native` or `crosssim` | `native` |
| `crosssim_config` | Optional CrossSim JSON parameter path or built-in configuration name | Empty/default CrossSim parameters |
| `analog_link_clock` | Clock for the tile-wide shared bidirectional 256-bit link | `1GHz` |
| `analog_compute_latency_cycles` | Compute latency in link-clock cycles | `100` |

The device implements `SetMatrix`, `LoadVector`, `Compute`, `StoreVector`, and
`MoveVector`. Every array owns an ordered four-command queue, but all arrays
share one half-duplex 256-bit link. A round-robin tile-wide arbiter advances
at most one eight-float32 beat per cycle. Independent array compute phases
still overlap. An uncontended `StoreVector` consumes
`ceil(analog_array_rows / 8)` link cycles; contention can increase its
completion latency.

`tests/analog_device_test.cpp` verifies the command semantics, MVM results,
array-to-array movement, error status, queue backpressure, shared-link
arbitration, bidirectional transfer-cycle accounting, and concurrent compute
on independent arrays.
`tests/analog_bridge_test.cpp` verifies the shared-memory ABI from an
independently mapped QEMU-side view.
`tests/sync_bridge_test.cpp` verifies fd 41 grant, analog-yield, memory-access,
aggregate memory-initialization, resume, task-start, task-finish, quantum-end,
and terminal-event state transitions from an independently mapped QEMU-side
view.
`tests/crosssim_backend_test.cpp` programs and computes through two independent
CrossSim backend instances and verifies that their local array state remains
separate.
`tests/analog_configuration.py` verifies that different tiles own the same
fixed array count and the same configured row and column counts. Backend
objects and array state remain independently owned by each tile. The proof
uses one SST global parameter set for the three geometry values and applies it
to every tile, then constructs the arrays with `analog_backend=crosssim`.

`tests/multi_tile_boot.py` creates two or more independently managed tiles in
one SST simulation. It defaults to four tiles and accepts the tile count from
`MITTENS_TEST_TILES`.

`tests/two_tile_bridge.py` connects two QEMU-backed tiles through
`merlin.linkcontrol` and a two-port Merlin router. Tile 0 sends a float32
payload to tile 1, tile 1 validates it and returns an acknowledgment, and both
guests exit only after the round trip succeeds.

`tests/performance_profile.py` repeats that exchange with detailed profiling
enabled. It verifies the live QEMU/SST packet metadata path and reconstructs
the injected-word, directional word-hop, endpoint-wait, and physical Merlin
link totals with `scripts/analyze-performance-profile.py`.

The intended component boundary is:

```text
bare-metal ELF <-> QEMU <-> mittens.tile <-> merlin.linkcontrol <-> Merlin mesh
```

`tests/memory-hierarchy-l1` separately verifies the optional path:

```text
QEMU tile <-> fd 41 <-> mittens.tile <-> StandardMem <-> private L1
```

The ownership boundaries and complete packet path are documented in
[`../../../docs/architecture.md`](../../../docs/architecture.md).
