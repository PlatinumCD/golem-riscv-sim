# Timing model and measurement limits

Platform v0 is functionally integrated but is not a cycle-synchronized CPU and
network simulation. This distinction is essential when interpreting SST output
or comparing runs.

## The two current time domains

QEMU and SST make progress independently:

```text
QEMU time domain                    SST time domain
----------------                    ---------------
executes guest instructions         processes discrete events
scheduled by the host OS            advances simulated timestamps
reaches MMIO at host-dependent time models Merlin links and routers
```

QEMU executes each RISC-V tile as quickly as the host permits. The current
launch does not use instruction quanta, stop/resume handshakes, or an SST CPU
timing model. Different QEMU processes may receive different amounts of host
CPU time.

`mittens.tile` registers an SST clock at `process_poll_frequency`, which
defaults to `1MHz`. On those SST ticks it:

- checks the shared bridge for guest transmissions;
- delivers due SST packets into the receive bridge; and
- checks whether the QEMU child has exited.

The shared bridge contains queue state but no simulated timestamp or executed
instruction count. Therefore, SST does not know how many guest instructions
occurred before a packet appeared in the transmit queue.

## What SST currently models

Once `mittens.tile` observes and injects a packet, Merlin models the network
portion in SST simulated time, including:

- topology and selected router ports;
- configured link latency and bandwidth;
- input and output buffering;
- crossbar and output-port contention; and
- backpressure through `SST::Interfaces::SimpleNetwork`.

The current shared 2D test topology configures 10 ns links, 1 GB/s link and
crossbar bandwidth, 4-byte flits, and 64-byte input and output buffers. Router
and link statistics are valid observations of that configured SST network
model after packet injection.

## What the reported completion time means

An SST line such as:

```text
Simulation is complete, simulated time: 14.8 ms
```

is not the RISC-V application runtime. It includes SST clock ticks spent
polling host QEMU processes. The number of ticks before a child reaches an MMIO
operation or exits can depend on host scheduling and host performance.

The completion timestamp must not be interpreted as:

- RISC-V CPU cycles;
- execution latency of a tile routine;
- end-to-end application performance;
- a cycle-accurate overlap of computation and communication; or
- a host-independent performance result.

A guest `rdcycle` observation, if provided by QEMU, would likewise not be
synchronized with SST or represent a modeled Golem microarchitecture.

## Measurements that are valid today

The current implementation and tests can support claims about:

- correct RISC-V program execution;
- correct MMIO and bridge behavior;
- packet source and destination at the SST endpoint;
- functional delivery and ordering for the tested traffic;
- selected Manhattan route and hop count;
- per-router packet and bit counts;
- modeled network stalls and idle time; and
- deterministic application results enforced by packet dependencies.

Network latency statistics describe the configured Merlin model between SST
injection and SST delivery. They do not include a synchronized model of the
guest computation before injection or after delivery.

## Consequences for current tests

The single-tile, communication, routed-mesh, and computation-pipeline proofs
are correctness tests. Their pass or fail result is meaningful. Small changes
in their final SST completion timestamp are not performance regressions.

Busy-wait loops such as `mesh_nic::receive()` may execute an arbitrary number
of times before SST places a payload in the receive bridge. Those iterations
are functionally harmless but currently have no defined relationship to SST
time.

## Required synchronization milestone

A synchronized design should make SST the time authority and introduce an
explicit execution protocol:

1. SST grants a tile a bounded instruction or timing quantum.
2. QEMU executes until the quantum ends, an MMIO boundary is reached, or the
   tile blocks.
3. QEMU reports progress and pauses.
4. SST advances the tile's modeled CPU time and processes due network events.
5. SST places any delivered payloads in the bridge.
6. SST resumes the tile for its next quantum.

QEMU supplies functional instruction execution, not a detailed pipeline or
cache timing model. Even after synchronization, SST will need an explicit CPU
timing policy—for example, an initial instructions-per-cycle model followed by
a more detailed microarchitectural model.

QEMU `-icount` may help make QEMU's internal virtual time deterministic, but it
does not by itself synchronize packet injection, blocking, and delivery with
the SST event schedule. The QEMU/SST handshake remains the required boundary.
