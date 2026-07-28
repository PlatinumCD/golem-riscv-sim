# Timing model

Platform v0.1 synchronizes every managed QEMU tile to the SST event schedule.
SST is the time authority; QEMU is the functional RISC-V instruction executor.

## CPU synchronization

Every tile owns a control bridge on QEMU file descriptor 41. SST grants QEMU
a bounded instruction quantum, QEMU executes under precise `-icount`, and QEMU
returns an event containing the number of instructions completed in that
grant. The initial CPU timing policy is:

```text
one retired RISC-V instruction = one cpu_clock cycle
```

The default `cpu_clock` is 1 GHz and the default
`sync_instruction_quantum` is 1,000 instructions. The quantum is a host
execution optimization, not a simulated delay: SST schedules the next control
event after the exact reported instruction count.

```text
SST                                      QEMU
---                                      ----
grant N instructions over fd 41  ----->  execute under precise icount
                                         stop at quantum or device boundary
receive reason + executed count  <-----  yield over fd 41
advance exactly that many cycles
process the boundary
resume or issue the next grant    ----->  continue
```

QEMU may yield before the end of a quantum for:

- a mesh transmission;
- a blocking mesh receive wait;
- an analog submission;
- an analog queue/completion wait; or
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
additional. Destination software cannot observe any cell data until Merlin
delivers the request.

The default deployment configuration is a 32-bit, 1 GHz link: 4 GB/s with
16 KiB cells. One full cell therefore occupies a link for 4096 ns. A 64-bit,
1 GHz experiment uses 8 GB/s and charges the same cell 2048 cycles without
changing its 4096 architectural words.

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

The `sync_instruction_quantum` therefore no longer determines idle receive
polling granularity. It still bounds ordinary instruction execution between
unrelated device boundaries.

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

This is a timed NIC-to-local-memory boundary, not a complete local memory
hierarchy. The model does not yet represent scratchpad capacity, banks,
CPU/DMA port contention, caches, TLBs, or DRAM. Those can replace or extend
the local endpoint without changing the route frame ABI.

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

Every analog array owns an independent bidirectional 256-bit link and ordered
four-entry queue. The common `analog_link_clock` advances those links, and
`analog_compute_latency_cycles` sets compute latency in that clock domain.

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
```

Different arrays progress concurrently. CrossSim host execution time is not
charged as simulated latency; CrossSim supplies numerical behavior while SST
supplies the modeled transfer and compute schedule.

## Valid measurements

The current implementation supports deterministic measurements of:

- retired instruction count under the one-instruction-per-cycle policy;
- CPU cycles between synchronized device boundaries;
- packet injection time and Merlin network timing;
- receive-DMA setup, bandwidth, serialization, queue pressure, and
  cross-tile overlap;
- analog transfer and configured compute cycles;
- overlap between independent analog arrays; and
- end-to-end Platform v0.1 simulated completion time.

Native and CrossSim runs with identical architectural behavior should have the
same simulated CPU timeline even if their host runtimes differ.

## Profiling

`mittens.tile` verbosity level 1 reports per-tile synchronized instruction,
stop-reason, network-word, and analog-operation counters at simulation finish.
The ResNet-18 deployment also supports an opt-in, cycle-level runtime profile:

```bash
MITTENS_RESNET18_RUNTIME_PROFILE=1 \
./tests/sculptor-resnet18-8x8/run-deployment.sh
```

That mode measures receive, receive-wait, transmit, blocked-transmit,
generated-task, and idle runtime steps with `rdcycle`. The reads and
accounting instructions perturb the simulated CPU timeline, so profile runs
are for attribution only. Official completion comparisons must use the
default uninstrumented build. The measured baseline and blocking-receive
comparison are documented in
[ResNet-18 8x8 performance profile](resnet18-profile.md).

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
