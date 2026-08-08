# Timing model

Platform v0.1 synchronizes every managed QEMU tile to the SST event schedule.
SST is the time authority; QEMU is the functional RISC-V instruction executor.

## CPU synchronization

Every tile owns a control bridge on QEMU file descriptor 41. SST grants QEMU
a bounded instruction quantum, QEMU executes under precise `-icount`, and QEMU
returns an event containing the total and vector instruction counts completed
in that grant. SST computes CPU issue cycles as:

```text
cpu cycles =
    max(ceil(total retired instructions / cpu_issue_width),
        retired vector instructions)
```

`cpu_issue_width` accepts 1, 2, or 4 and defaults to 1. The vector term limits
the tile to at most one vector issue per cycle even when the scalar front end
is dual- or quad-issue. QEMU executes RVV semantics, but SST does not yet
assign vector-length-dependent latency, dependencies, or resource occupancy.
RVV results and scalar/vector issue throughput are modeled; RVV performance
is not yet a detailed vector-pipeline model.

The default `cpu_clock` is 1 GHz and the component default
`sync_instruction_quantum` is 1,000 instructions. SST schedules the next
control event after the issue cycles calculated from the exact reported
counts. The quantum adds no synthetic delay, but it is also the temporal
lookahead bound between independently executing QEMU and SST: QEMU cannot
observe an SST delivery that becomes ready in the middle of a grant until it
reaches a device boundary or the grant ends. It is therefore an accuracy and
host-throughput parameter for code whose control flow depends on asynchronous
device state, and every model-level result must record it.

```text
SST                                      QEMU
---                                      ----
grant N instructions over fd 41  ----->  execute under precise icount
                                         stop at quantum or device boundary
receive reason + total/vector counts <- yield over fd 41
advance by calculated issue cycles
process the boundary
resume or issue the next grant    ----->  continue
```

QEMU may yield before the end of a quantum for:

- an accepted deployment-burst descriptor or a full legacy transmit ring;
- a blocking mesh transmit wait;
- a blocking mesh receive wait;
- an analog submission;
- an analog queue/completion wait;
- a timed RAM data access when `memory_backend=memhierarchy`;
- normal guest completion through the SiFive test finisher; or
- another explicitly modeled device boundary.

Counts are monotonic within one grant even when QEMU internally rebases its
icount counters. The fd 41 bridge accumulates those internal segments before
reporting progress to SST. Two boundaries at the same retired-instruction
count are scheduled at the same SST time; control handshakes never add a
synthetic CPU cycle.

A normal finisher write publishes `GUEST_EXIT` before QEMU shuts down. SST
schedules the terminal event using its exact instruction count and reaps the
child at that event, so host process-exit observation cannot change the
simulated completion time.

Scalar/vector issue occupancy carries across `QUANTUM_END` yields. Otherwise,
rounding `ceil(instructions / issue width)` independently at each host quantum
would turn the nonarchitectural batching size into a timing parameter. A real
fd-41 device or task boundary closes the current issue interval and starts a
new one.

Instruction-only regions are exactly quantum invariant. Explicit fd-41
device boundaries are also timestamped exactly. Receive data that becomes
ready while QEMU is inside a grant is the remaining exception: because the
current co-simulation has no rollback, a coarse grant can allow QEMU to take
an empty-receive software path that a finer grant would have avoided.

`tests/platform/cpu-timing` verifies this rule with widths 1/2/4 and quanta
37/1,000. Its 18 region checks have zero-cycle error, including explicit
counting of `vsetivli`, and each width has identical timestamps and completion
time under both quanta.

## Bridge roles

The three inherited descriptors have distinct responsibilities:

| Descriptor | Role |
| ---: | --- |
| 41 | Execution grants, yields, stop reasons, and resume handshakes |
| 42 | Mesh NIC packet data |
| 43 | Analog command, operand, and result data |

Descriptors 42 and 43 are data planes. They do not advance or resume QEMU.
For example, an analog instruction first publishes its command and payload in
fd 43, then yields once through fd 41 with `ANALOG_SUBMIT`. SST reads fd 43,
models the operation, and resumes QEMU through fd 41. A blocking
`StoreVector` remains held until its fd 43 result is complete.

The deployment NIC follows the same control/data separation. QEMU snapshots a
burst into fd 42, then publishes a zero-wait `NIC_TRANSMIT` descriptor
doorbell through fd 41. SST advances to the exact reported instruction
boundary, drains fd 42, and resumes QEMU immediately when the bounded ring has
space. If guest software observes no burst slot, `TX_WAIT` rechecks the ring
inside QEMU and publishes `NIC_TRANSMIT_WAIT` only when it is still full. The
hart then remains stopped until SST frees a slot. This makes transmit timing
independent of `sync_instruction_quantum` without inventing a control latency.

## Optional private-L1 memory timing

Every tile selects one of two data-memory backends:

| `memory_backend` | Behavior |
| --- | --- |
| `native` | QEMU accesses its private RAM directly; no cache or lower-memory delay is added |
| `memhierarchy` | Each QEMU RAM data access yields through fd 41 and completes through the tile's StandardMem path |

`native` is the default and preserves existing simulations. In
`memhierarchy` mode, the SST configuration must attach
`memHierarchy.standardInterface` to the tile's `memoryIF` slot. The first
proof connects that interface to one 32 KiB, four-way, 64-byte-line private L1
and a simple 50 ns lower-memory timing backend:

```text
guest load/store
      |
      | physical address, byte size, read/write
      v
fd 41 MEMORY_ACCESS
      |
      v
mittens.tile -> StandardMem -> private L1 -> timing memory controller
      ^                              |
      `--------- response -----------'
      |
      v
resume QEMU and complete the functional RAM access
```

QEMU remains the sole owner of guest data. SST models only the completion
time; read payloads returned by StandardMem are intentionally ignored and
write payloads are placeholders. Instruction fetches, MMIO, and QEMU device
accesses do not enter this path.

The current bridge permits one outstanding CPU memory access per tile because
QEMU blocks until the StandardMem response. Different tiles still overlap,
and every tile owns a distinct L1 instance. This establishes real cache
hit/miss and lower-memory latency without claiming out-of-order misses,
prefetching, instruction-cache timing, TLB timing, cache/DMA coherence, or
scratchpad behavior.

Large deployment images perform millions of data accesses while installing
their task and analog-array state. An optional initialization phase preserves
the byte volume but aggregates its host synchronization:

```text
QEMU boot and runtime.boot()
        |
        | count reads/writes locally; do not yield per access
        v
guest complete_memory_initialization()
        |
        | one fd 41 MEMORY_INIT_COMPLETE event
        v
SST delay = setup cycles + ceil(total bytes / bytes per cycle)
        |
        v
resume QEMU; every later access uses StandardMem/private L1
```

The default aggregate rate is 32 bytes per cycle, corresponding to a 256-bit
initialization path, with a two-cycle setup cost. Both values are SST
parameters. This is an explicit phase model: it does not populate L1 cache
state, and post-marker cache hits and misses are measured independently. The
profile reports the handshake count, access count, read and write byte counts,
and charged initialization cycles.

`tests/memory/timing` independently verifies the cache boundary.
For the documented L1, links, and 50 ns lower-memory backend, it observes exact
five-cycle hits and 61-cycle misses. Its conflict/LRU sequence matches four
hits, six misses, and 386 wait cycles; its capacity sequence matches one hit,
514 misses, and 31,359 wait cycles.

## Mesh timing

The legacy `TX_DATA` path publishes one 32-bit packet in fd 42. The deployment
runtime instead submits up to 4096 contiguous words through one DMA-style
fd 42 burst slot. Mittens creates one bounded Merlin request. Routing,
buffering, contention, credits, and backpressure remain modeled even though
the host handles one event per burst rather than one per word.

Mesh payloads remain sequences of architectural 32-bit words, while the
physical SST link width and clock are configurable. `build_mesh()` accepts
`mesh_link_width_bits`, which must be a positive multiple of 32, and
`mesh_link_clock`. Merlin bandwidth is derived as:

```text
link bandwidth = mesh_link_width_bits * mesh_link_clock
link cycles per timing cell =
    ceil((network_cell_words * 32) / mesh_link_width_bits)
```

Timing cells are padded to a complete physical beat. A partial final request
is additionally padded to the configured cell size. Arbitration occurs at
cell boundaries; configured link/router latency and contention are
additional.

Model-level deployments use `network_cell_words=1`: one timing cell is one
architectural 32-bit word, so a 512-word request consumes 512 cycles on a
32-bit, 1 GHz link. The independent `network_buffer_cells=16384` setting
preserves 64 KiB of router capacity. The 4,096-word maximum fd-42 host burst
is a batching boundary and does not widen the timing cell.

Merlin notifies a `SimpleNetwork` endpoint when the request's head flit
arrives, while reserving downstream link bandwidth for the complete request.
Mittens therefore applies an explicit tail-completion boundary before placing
the burst in fd 42:

```text
packet link cycles =
    ceil(packet_words * 32 / mesh_link_width_bits)

serialization start =
    max(Merlin head-arrival time,
        destination link's next-available time)

NIC-visible time =
    serialization start
    + (packet link cycles - 1) * mesh-link period

destination link next-available time =
    serialization start
    + packet link cycles * mesh-link period
```

Without that boundary, a lone multiword packet would become visible after
only its head latency even though Merlin correctly kept the physical link
occupied for all remaining flits. Destination software and receive DMA can
now observe a burst only after its final physical beat. The per-destination
next-available time also preserves physical packet order: a later short packet
cannot complete ahead of an earlier long packet merely because its calculated
tail delay is shorter.

The controlled 32-bit, 1 GHz, 10 ns-link configuration has the exact
closed-form result:

```text
uncontended head cycles = 35 + 12 * (Manhattan hops - 1)
uncontended completion =
    head cycles + packet_words - 1
```

The one-hop constant contains three 10-cycle SST links plus five
endpoint/router pipeline cycles. Each additional hop contributes one
10-cycle link and two router cycles. Under equal-size one-hop incast,
contender `rank` receives the output after:

```text
head[rank] = 35 + rank * packet_words
completion[rank] = 35 + (rank + 1) * packet_words - 1
```

`tests/network/timing` checks this law with 21 exact timestamp
comparisons across packet size, Manhattan distance, and two/four-source
contention.

The corrected deployment configuration is a 32-bit, 1 GHz link: 4 GB/s with
one 32-bit word per timing cell. One cell occupies the link for one cycle; a
64-bit, 1 GHz experiment transfers two cells per cycle. The independent
16,384-cell buffer preserves 64 KiB of capacity.

### Transmit doorbell and backpressure

An accepted deployment burst has two distinct times:

```text
descriptor visible = exact fd-41 NIC_TRANSMIT boundary
network service     = Merlin serialization, routing, buffering, and contention
```

The descriptor doorbell contributes no synthetic cycle. It prevents QEMU
from running to an unrelated quantum boundary before SST sees a partial
transmit ring. When all four burst slots are occupied, runtime progress
returns `WaitForTransmit`; the platform writes `TX_WAIT`, and the hart sleeps
until the matching burst ring has space. The status check and wait publication
occur in one QEMU MMIO operation, preventing a lost wakeup.

The performance profile records every actual blocked interval in
`tile-<id>-transmit-blocked.csv`, including route, execution, destination,
transfer kind, word count, start/finish ticks, retry count, and maximum queue
occupancy. Per-tile summaries include total blocked ticks, events, retries,
and maximum occupancy.

The receive interface provides an `RX_WAIT` doorbell. Guest software first
checks `STATUS.RX_VALID` and the receive-DMA completion bit, then writes
`RX_WAIT` only when neither ordinary data nor a completion is available. QEMU
rechecks the receive queues, services eligible DMA bursts, and rechecks the
completion queue inside the MMIO write to prevent a lost wakeup. If data or a
completion became visible between the status read and the write, QEMU
continues immediately. Otherwise it yields `NIC_RECEIVE_WAIT` through fd 41.

SST schedules that stop after the exact number of CPU instructions reported
by QEMU. At the scheduled stop timestamp it services any network data already
delivered to the endpoint. If the receive bridge remains empty, the tile
stays stopped. A later Merlin delivery places the cell in fd 42 and resumes
the hart at that delivery timestamp. The control handshake adds no synthetic
cycle:

```text
receive observation = max(
    scheduled CPU time of the RX_WAIT boundary,
    Merlin delivery time of the next receive cell
)
```

Once software has published `RX_WAIT`, its wakeup is event driven and does
not wait for an ordinary quantum end. The quantum still bounds how far QEMU
may execute before publishing that wait, however. If network data becomes
ready during the preceding grant, a coarse grant can change whether software
observes it before or after a runtime scan. The controlled Epoch D fanout
audit converges at 36.188 us for 1,000- and 100-instruction grants, versus
36.711 us for 100,000 and 1,000,000 instructions; traffic is identical.

### Receive DMA and timing

After software decodes a five-word route header, it may register the
destination tensor range with the NIC. Subsequent fd-42 payload bursts are
copied into that guest range and generate a completion instead of requiring
one guest MMIO load per 32-bit word:

```text
source RAM -> fd 42 -> Merlin mesh -> RX DMA engine -> destination RAM
                         timed             timed
```

This changes CPU instruction work, not mesh work. Packet count, architectural
word count, route, physical width, link serialization, buffer pressure, and
contention remain identical.

Descriptor submission is an exact fd-41 `NIC_RX_DMA_SUBMIT` boundary carrying
source, route ID, and word count. Each tile owns one SST-clocked DMA channel,
so its bursts serialize while DMA engines on different tiles overlap. A
burst becomes guest-visible only when an SST self-event authorizes it at:

```text
start = max(current rx_dma_clock cycle, channel next-available cycle)
transfer = ceil(burst_word_count * 32 / rx_dma_width_bits)
completion = start + transfer + first_burst(rx_dma_setup_cycles)
```

Defaults are a 1 GHz clock, 256-bit width, eight setup cycles per descriptor,
and four queued receive bursts. The queue is finite and applies endpoint
backpressure. QEMU performs the functional guest-RAM write only after
authorization, so software cannot observe the payload early.

This is a timed NIC-to-local-memory boundary. The optional private data-L1
backend times CPU loads and stores separately, but the two paths do not yet
share ports or maintain cache/DMA coherence. Scratchpad capacity, banks,
instruction caches, TLBs, and a detailed DRAM backend remain future work.

## Task trace timing

The optional task tracer uses the same fd-41 instruction boundary as the NIC
and analog devices. `TASK_START` is emitted immediately before
`Task::execute`; `TASK_FINISH` is emitted immediately after it returns. SST
schedules each marker after the exact instructions retired since the previous
stop, then records `getCurrentSimCycle()` using the simulation's configured
time base.

Trace records use `(tile_id, task_id, execution_id)` as their identity.
Because SST writes them at global simulation time, timestamps from different
QEMU processes are directly comparable. Per-tile raw CSV files avoid
corruption from QEMU UART and SST text sharing stdout.

The marker executes a small number of additional guest MMIO instructions and
adds an fd-41 handshake. A traced result is therefore diagnostic rather than
the official benchmark. The receive-DMA 8x8 ResNet-18 trace completed at
1.278541685 simulated seconds and produced 123 matched task intervals.

## Analog timing

Every analog array owns an ordered four-entry queue. All arrays on one tile
share one bidirectional, half-duplex 256-bit link. The
`analog_link_clock` advances that link, and
`analog_compute_latency_cycles` sets the latency of each independent array
compute engine in that clock domain.

The command timing rules are:

1. `SetMatrix` and `LoadVector` snapshot guest input, publish it in fd 43, and
   yield through fd 41.
2. `Compute` publishes and yields through fd 41.
3. These asynchronous commands resume after their selected queue accepts
   them.
4. `StoreVector` yields through fd 41 and remains stopped until SST completes
   the selected array's output transfer.
5. SST writes the result to fd 43 and resumes QEMU through fd 41; QEMU then
   copies the result into private guest RAM.

One link beat carries eight 32-bit words. Transfer costs are:

```text
SetMatrix:  ceil((rows * columns) / 8) link cycles
LoadVector: ceil(columns / 8) link cycles
StoreVector: ceil(rows / 8) link cycles
MoveVector: 2 * ceil(rows / 8) link cycles
```

At most one transfer beat crosses the shared link per tile per link cycle.
Contending arrays receive beats in deterministic round-robin order. Compute
phases on different arrays still progress concurrently and may overlap the
winning transfer beat. `MoveVector` requires two transfers: source array to
tile, followed by tile to destination array. CrossSim host execution time is
not charged as simulated latency; CrossSim supplies numerical behavior while
SST supplies the modeled transfer and compute schedule.

`tests/analog/timing` checks this boundary end to end from real
bare-metal Golem instructions through QEMU, fd 43, fd 41, and SST. An
independent reference scheduler predicts every service-phase timestamp from
the observed command arrivals. For 9x9 arrays and an eight-cycle compute
latency, the single-array case matched 23/23 active cycles and 15/15 link
beats; the dual-array case matched 37/37 active cycles and 30/30 link beats,
including 14 cycles of contending transfer demand and two cycles of
independent compute overlap.

## Valid measurements

The current implementation supports deterministic measurements of:

- retired instruction count under the one-instruction-per-cycle policy;
- CPU cycles between synchronized device boundaries;
- packet injection time and Merlin network timing;
- receive-DMA setup, bandwidth, serialization, queue pressure, and
  cross-tile overlap;
- analog transfer and configured compute cycles;
- shared-link contention and overlap between independent analog computes; and
- end-to-end Platform v0.1 simulated completion time.

Native and CrossSim runs with identical architectural behavior should have the
same simulated CPU timeline even if their host runtimes differ.

## Performance attribution

Every tile supports a machine-readable profile independent of the existing
verbose console counters:

| Mode | Output | Simulated-time effect |
| --- | --- | --- |
| `off` | No profile files | None |
| `summary` | One finish-time counter file per tile | None |
| `trace` | Summary plus wait, packet, receive-DMA, analog, memory, and task event files | Task markers add guest instructions and fd-41 boundaries |

The next RA-tree multi-tile deployment runner will configure all tiles and run
the system-level analyzer automatically. Existing platform tests validate the
profile format independently of a neural-network deployment.

`summary` is the correct mode for low-overhead counter collection. `trace` is
the diagnostic mode used to locate a critical chain. Host-side CSV writes do
not advance SST time, but task start/finish markers execute guest MMIO and
therefore perturb the traced program. Official performance numbers must come
from an untraced run; the traced run explains those numbers.

The raw directory contains:

| File | Meaning |
| --- | --- |
| `tile-N-summary.csv` | Retired instructions, modeled CPU cycles, network totals, device activity, and wait totals |
| `tile-N-network.csv` | Packet ready, injection, and arrival timestamps plus route/execution identity |
| `tile-N-receive-dma.csv` | Receive-DMA schedule and completion records |
| `tile-N-analog.csv` | Queue, input transfer, compute, output transfer, move, and completion phases |
| `tile-N-memory.csv` | Timed StandardMem issue and response events |
| `tile-N-waits.csv` | Exact fd-41 stop intervals by reason |
| `tile-N.csv` | Task start and finish markers in trace-enabled deployment ELFs |

`scripts/analyze-performance-profile.py` joins those files with Merlin router
statistics and, when available, `deployment-routes.csv`. It writes:

- `summary.json` and `summary.csv`;
- `network-packets.csv` and logical `routes.csv`;
- `receive-dma.csv`, `analog-operations.csv`, and `memory-requests.csv`;
- `waits.csv` and `link-statistics.csv`; and
- `critical-path.csv`.

The route manifest is extracted from partitioned Sculptor MLIR by
`scripts/extract-deployment-routes.py`. It preserves the global source and
destination task IDs, route ID, resource ID, byte count, payload words, and
expected Manhattan distance. The analyzer uses route arrival/DMA readiness
to connect task intervals across tiles. Within a tile, it adds the preceding
task as a serialization dependency, selects the latest ready predecessor for
each observed task, and backtracks from the final finishing task. This is an
observed execution critical chain, not a static estimate of every possible
task-graph path.

The network quantities are deliberately separate:

```text
injected words = each architectural 32-bit word counted once at its source

directional word-hops =
    sum(packet words * source-to-destination Manhattan hops)

physical link bits =
    sum of Merlin send_bit_count on non-endpoint router ports
```

The first measures application traffic, the second measures topology-weighted
communication work, and the third records what the modeled links actually
carried. Packet latency, endpoint queue time, DMA activity, device activity,
and per-tile waits are sums of intervals and may overlap across tiles. They
must not be added together and called end-to-end time; `critical-path.csv`
exists to expose the causally limiting sequence.

With `memory_backend=native`, QEMU RAM has no separately modeled latency, so
`memory.modeled` is false. With `memhierarchy`, the request report contains
the actual StandardMem response latency. Initialization batching remains a
single explicitly charged summary event rather than millions of fabricated
per-access records.

## Runtime microprofile

`mittens.tile` verbosity level 1 reports per-tile synchronized instruction,
stop-reason, network-word, and analog-operation counters at simulation finish.
A future RA-tree neural-network deployment can enable cycle-level runtime
profiling. Such instrumentation measures receive, receive-wait, transmit,
blocked-transmit, generated-task, and idle runtime steps with `rdcycle`.
It perturbs the simulated CPU timeline, so profile runs are for attribution
only. Official completion comparisons must use an uninstrumented build.

## Limits

This is a synchronized functional CPU model, not a cycle-accurate RISC-V
microarchitecture. It does not yet model:

- pipeline width, hazards, branch prediction, or instruction-dependent CPI;
- cache, TLB, or DRAM stalls;
- NIC interrupts—the current receive mechanism is an explicit blocking
  doorbell;
- detailed UART timing; or
- operating-system scheduling.

`rdcycle` remains QEMU's architectural counter and is not the public source of
SST timestamps. Timing analyses should use SST time and the fd 41 instruction
statistics. A future CPU model can replace the one-instruction-per-cycle rule
without changing the bridge separation or device-boundary protocol.
