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

A `TX_DATA` MMIO write publishes one 32-bit packet in fd 42 and yields with
`NIC_TRANSMIT`. SST therefore injects that packet at a defined CPU instruction
boundary. Merlin then models routing, link latency, bandwidth, buffering,
contention, and backpressure in SST time.

The current receive interface is polling MMIO. An empty status read is not
itself a blocking fd 41 event because the same status register also reports
transmit readiness. A receiving guest runs until its next synchronization
boundary, at most one instruction quantum, and SST then moves any delivered
packets into fd 42. Receive-observation timing is consequently deterministic
but currently has up to `sync_instruction_quantum` instructions of polling
granularity.

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
- analog transfer and configured compute cycles;
- overlap between independent analog arrays; and
- end-to-end Platform v0.1 simulated completion time.

Native and CrossSim runs with identical architectural behavior should have the
same simulated CPU timeline even if their host runtimes differ.

## Limits

This is a synchronized functional CPU model, not a cycle-accurate RISC-V
microarchitecture. It does not yet model:

- pipeline width, hazards, branch prediction, or instruction-dependent CPI;
- cache, TLB, or DRAM stalls;
- interrupts or a blocking NIC receive register;
- detailed UART timing; or
- operating-system scheduling.

`rdcycle` remains QEMU's architectural counter and is not the public source of
SST timestamps. Timing analyses should use SST time and the fd 41 instruction
statistics. A future CPU model can replace the one-instruction-per-cycle rule
without changing the bridge separation or device-boundary protocol.
