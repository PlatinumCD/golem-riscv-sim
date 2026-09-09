# Mittens

Mittens is the SST element for QEMU-backed, bare-metal RISC-V compute tiles.

The `mittens.tile` component owns the tile configuration and SST lifecycle and
exposes optional `SST::Interfaces::SimpleNetwork` and
`SST::Interfaces::StandardMem` subcomponent slots. When populated, the
component forwards the SST `init`, `setup`, and `finish` phases to those
interfaces.

The element also supplies `mittens.wormholeNIC` and
`mittens.wormholeRouter`. These components implement the default model network.

The router has four directional outputs and one shared local-ejection output.
It accepts one, two, or four local TX injection lanes in addition to its four
directional inputs. It uses deterministic XY routing, finite input buffers,
per-flit credits, and round-robin arbitration. More TX lanes do not duplicate
the physical links or scratchpad resources.

Each request becomes a stream of 32-bit flits. The output stays reserved until
the packet tail departs.

The network interface returns ejection credits when the tile removes a
complete request. A full tile receive path can therefore stop upstream links.

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

With `memory_init_batching` and a deployment-wide
`memory_init_barrier_tiles`, `qemu_ready_set_workers` may be greater than one.
The production launcher enables this only after validating that the frozen
materialization audit has no unowned, multiply owned, read-before-produced,
cross-epoch-direct, or unclassified regions, and passes that audit's digest to
every participating tile.
The first independent grant for every active tile then executes in a bounded
host worker pool. Workers only wait on their tile's fd 41 bridge; the SST event
thread validates every result and commits the complete batch in modeled
delivery-tick, tile-ID, grant-epoch, and event-sequence order. Any mixed
frontier, missing tile, unexpected stop reason, or worker failure terminates
the run before a partial commit. Optional `qemu_runtime_ready_set` and
`qemu_local_lookahead` also support runtime captures and private-SPM lookahead;
these are host execution controls, not additional modeled CPU issue bandwidth.
See [the execution contract](execution/R8.md) for validated ordering, cancellation,
and the limits of ready-set preview. CPU event readiness never bypasses its
instruction-delivery or pending device-service deadline.

After each tile's analytically timed initialization transfer completes, the
tile sends one arrival to the modeled
`mittens.memoryInitializationBarrierController` and remains blocked without
polling. The controller accepts each active tile exactly once and sends each
tile exactly one release. `release_cycles` is the latency from the last
arrival to the first release; `release_tiles_per_cycle` is the controller's
finite release bandwidth. Production uses one release per 1 GHz controller
cycle, so a 400-tile deployment spans at most 399 additional cycles rather
than waking every tile on one simulated frontier. Missing links, duplicate or
late arrivals, duplicate releases, and incomplete terminal state fail closed.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `cpu_clock` | SST clock for synchronized instruction cycles | `1GHz` |
| `cpu_issue_width` | Scalar front-end issue width; one of 1, 2, or 4 | `1` |
| `sync_instruction_quantum` | Maximum instructions in one QEMU grant | `1000` |
| `qemu_ready_set_workers` | Host workers for deterministic same-frontier initial QEMU grants; `1` is the serial reference | `1` |
| `qemu_ready_set_independence_proof` | Frozen materialization-audit digest required by multi-worker initial grants | Empty |
| `memory_backend` | Data-memory timing backend: `native`, `memhierarchy`, or cacheless `streaming` | `native` |
| `memory_init_batching` | Aggregate pre-marker memory traffic into one fd 41 initialization event | `false` |
| `memory_access_batching` | Group the fd 41 transport for runtime memory accesses | `false` |
| `scratchpad_access_batching` | Group fd 41 transport while replaying every scratchpad access through the SPM timing model | `false` |
| `scratchpad_access_run_compaction` | Encode adjacent same-instruction SPM accesses as an exact contiguous run; requires scratchpad batching | `false` |
| `memory_event_batching` | Attach a pending memory batch to its following semantic fd 41 event | `false` |
| `analog_command_batching` | Batch proven nonblocking analog submissions on fd 41 while replaying every command at its original modeled CPU boundary | `false` |
| `memory_access_batch_records` | Set the maximum logical accesses in one runtime batch | `16` |
| `memory_load_queue_entries` | Limit grouped vector and scalar load requests | `8` |
| `memory_store_buffer_entries` | Limit incomplete timed stores | `1` |
| `memory_init_bytes_per_cycle` | Aggregate initialization transfer rate | `32` |
| `memory_init_latency_cycles` | Fixed setup cost for the aggregate initialization transfer | `2` |
| `memory_init_barrier_tiles` | Active deployment tiles participating in the one-shot initialization barrier; `0` disables the tile endpoint | `0` |
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
publishes guest RAM data accesses through fd 41. Mittens sends matching
StandardMem requests. Proven-independent consecutive scalar loads can use the
configured load queue. Other scalar loads wait for their responses. Stores can
remain in the configured store buffer. Cache-line fragments from one vector
load can also use the load queue together.

The functional load or store still uses QEMU RAM. The attached StandardMem
system owns timing, not guest bytes. A cache is not required: the deadline
regression connects the interface directly to a RAM controller. Native mode
creates no StandardMem traffic; the separate local-SPM model remains available.

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
configured StandardMem path.

Runtime access batching is valid only with `memory_backend=memhierarchy`.
QEMU writes ordered access records to the fd 41 shared-memory region.
Each record contains its instruction count, vector count, address, size,
direction, PC, and return address. Standard scalar load records also contain
register dependency masks and the instruction length. SST replays each record
through the same StandardMem path. Each request uses the configured memory
timing model.

Mittens combines adjacent elements from one vector instruction by cache line.
It can issue the resulting line requests together. It can also group adjacent
scalar loads when the later address does not depend on an earlier load result.
It preserves access order when the dependency data is absent or incomplete.

The protocol supports a maximum of 1,024 records. The
`memory_access_batch_records` parameter sets a smaller active limit. QEMU
sends a partial batch at each quantum or external event. Atomic accesses stay
synchronous.

Set `memory_access_batching=true` to use this transport. A larger active limit
decreases host synchronization. It also increases functional lookahead before
SST replays the records. Use the standard transport for reference results.

Set `scratchpad_access_batching=true` to use the same bounded transport for
CPU accesses to tile SPM. Scratchpad and ordinary-memory records are never
mixed in one batch. SST preserves record order, instruction boundaries, and
every read/write. A normal record calls
`ScratchpadTimingModel::scheduleCPU`; an enabled
`scratchpad_access_run_compaction` record calls the exact contiguous-run path
and expands `repeat_count` into logical accounting. An aligned eight-element
e32 run is tagged as one 32-byte CPU transaction, so the fast path schedules
one 256-bit SPM beat while retaining eight logical guest elements. Other runs
use the exact contiguous-run timing path. The configured batch limit always
counts logical accesses, which preserves the uncompacted synchronization
boundary. External task, DMA, NoC, analog, fence, atomic, and quantum events
flush the batch before their architectural action.

Set `memory_event_batching=true` to attach a pending batch to the semantic
event that would immediately follow its standalone flush. SST replays the
batch before handling that event. This reduces fd 41 events but is an
independent transport optimization; it does not alter logical memory work.

When `networkIF` is attached, the component creates the fd 42 data bridge for
the custom QEMU `mittens-nic` device. A transmit publishes data on fd 42 and
yields through fd 41. See
[the repository timing notes](../../docs/timing-model.md) for modeled
timing and remaining CPU-model limits.

Each tile owns its current receive DMA frontend. Same-tile payload bursts serialize;
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

`SetMatrix` accepts the compact valid-shape bridge form. It validates the
packed row and column counts against the physical geometry, charges only the
valid float32 words on the shared link, then constructs a zero-padded physical
matrix before invoking either numerical backend. Unknown command flags,
noncanonical descriptor bits, out-of-range shapes, and payload-length
mismatches fail before backend state changes.

Set `analog_command_batching=true` to remove repeated host fd 41 rendezvous
from consecutive nonblocking analog submissions. QEMU retains every immutable
command in its ordinary fd 43 slot and transports only the exact instruction
boundary plus array/sequence token in a bounded batch. SST accepts each token
in order and replays it through the same link, queue, compute, completion, and
profiling paths. A timed memory access, DMA, wait, blocking submission, or
unsupported command flushes the batch, so the option changes host transport
only and does not approximate modeled analog work.

`tests/analog_device_test.cpp` verifies the command semantics, MVM results,
array-to-array movement, error status, queue backpressure, shared-link
arbitration, bidirectional transfer-cycle accounting, and concurrent compute
on independent arrays.
`tests/analog_bridge_test.cpp` verifies the shared-memory ABI from an
independently mapped QEMU-side view.
`tests/sync_bridge_test.cpp` verifies fd 41 grant, analog-yield, memory-access,
aggregate memory-initialization, resume, task-start, task-finish, quantum-end,
epoch-barrier arrival, and terminal-event state transitions from an
independently mapped QEMU-side view. `tests/run-epoch-barrier-test.sh` covers
modeled serial/two-thread release, explicit idle contributions, absent tiles,
and exact duplicate/stale/future diagnostics. The runtime epoch-barrier test
adds two real QEMU guests, cross-epoch global-RAM visibility, and concurrent
SST-process isolation.
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
bare-metal ELF <-> QEMU <-> mittens.tile <-> mittens.wormholeNIC
                                              <-> mittens.wormholeRouter mesh
```

`tests/wormhole_network.py` sends two simultaneous 16-word packets through a
shared output. It checks tail delivery and switch-arbitration delay.

The same test gives identical packet times with one or two SST threads.

`tests/cpu_memory_deadline.py` verifies early-response and stale-wakeup handling
with a cacheless StandardMem connection and the tile-local scratchpad. The
deleted private-L1/L2 tests are not part of this workspace's hardware gates.

The ownership boundaries and complete packet path are documented in
[the repository architecture notes](../../docs/architecture.md).

For refactoring ownership, build provenance, accepted comparisons and known
measurement limitations, start with [the src2 workspace guide](../../docs/hardware/history/source-refactor-notes.md),
[the task queue](../../docs/hardware/task-queue.md), and
[the measurement contract](profiling/MEASUREMENT_CONTRACT.md). The complete
tile parameter defaults live in `configuration/tileParameters.h`; the summary
tables above are not a second authoritative configuration definition.
