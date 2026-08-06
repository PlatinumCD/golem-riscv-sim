# Component Timing Validation Audit

**Date:** 2026-07-30  
**Scope:** CPU/RVV, analog device, and mesh network components exercised by
the GPT-2 scheduling sweep  
**Result:** Partial pass; the network timing gate is closed, while
operation-specific RVV and end-to-end analog timing remain open

## Summary

| Component | Current result | Status |
|---|---|---|
| Scalar issue-width model | Exact agreement at widths 1, 2, and 4 | Pass |
| RVV functional execution | VLEN=256 float32 vector operation completed correctly | Pass |
| RVV operation-specific latency | No instruction-class latency/occupancy model | Not validated |
| Analog shared-link model | Exact beat, arbitration, overlap, and completion assertions | Pass |
| Analog backend functionality | Native and independently owned CrossSim instances passed | Pass |
| Mesh routing | One-hop and four-hop routes followed the expected topology | Pass |
| Network word accounting | Injected words and directional word-hops matched exactly | Pass |
| Network closed-form latency | 21/21 isolated head/completion timestamps matched exactly | Pass |
| Router contention timing | Two- and four-source one-hop incast serialized exactly one packet per contender | Pass |

## CPU and RVV

The RISC-V vector microtest was executed at scalar issue widths 1, 2, and 4.
Every run retired exactly 589 instructions, including eight RVV instructions.
The test uses one synchronization interval, so the declared CPU timing rule is:

```text
cycles = max(ceil(retired_instructions / scalar_issue_width),
             vector_instruction_count)
```

| Issue width | Predicted cycles | Measured cycles | Error |
|---:|---:|---:|---:|
| 1 | 589 | 589 | 0% |
| 2 | 295 | 295 | 0% |
| 4 | 148 | 148 | 0% |

The functional test also confirmed RVV 1.0 execution with VLEN=256 and a
float32 vector addition.

This validates implementation of the current issue-throughput abstraction. It
does **not** validate realistic latency, occupancy, dependencies, or bandwidth
for individual vector operations. RVV instructions currently constrain issue
to at most one vector instruction per cycle but do not receive distinct
operation-specific timing.

## Analog Device

The Mittens element test suite passed all host-side analog assertions.
The tested device contains two 9×9 arrays, four queue entries per array, one
shared bidirectional 256-bit link, and four-cycle independent compute engines.

The test established:

- eight float32 words per shared-link beat;
- 81 words require 11 beats;
- two contending 81-word matrix transfers complete in 22 link cycles under
  deterministic round-robin arbitration;
- two nine-word vector loads consume four total shared-link beats;
- independent four-cycle computes overlap;
- stores contend for the same shared link;
- opposite directions serialize on the half-duplex link;
- `MoveVector` crosses the link in both directions;
- the primary set/load/compute/store sequence completes after exactly 33
  device cycles and 30 link beats;
- the extended move, recompute, store, and opposite-direction sequence reaches
  its exact asserted cycle and beat counts;
- native float32 MVM outputs are correct; and
- separate CrossSim backend instances retain independent array state and
  produce the expected outputs.

The analog timing implementation therefore agrees exactly with its declared
device-level model. A final Epoch C suite should additionally expose the same
predicted-versus-measured table through a complete QEMU/fd-41/SST execution for
each operation.

## Network

Three current proofs were rerun:

1. One 512-word payload route plus five framing words.
2. Twenty-four identical routes from one source to one destination.
3. The 3×3 tile-0-to-tile-8 route and acknowledgment.

| Trial | Injected words | Directional word-hops | Analytical raw-link serialization | End-to-end simulated time |
|---|---:|---:|---:|---:|
| One route | 517 | 517 | 517 ns | 26.010 us |
| Twenty-four routes | 12,408 | 12,408 | 12.408 us | 214.763 us |

The 24-route run recorded 179.470 us of transmit blocking from bounded endpoint
capacity. Both runs reported zero router stalls because all traffic originated
at one source and followed one path.

The four-hop 3×3 proof delivered the correct payload and acknowledgment and
verified the exact XY route through the expected routers and ports.

These tests validate payload accounting, routed word-hops, endpoint
backpressure, and topology. They do not isolate network latency:

- end-to-end completion includes guest instructions, runtime dispatch,
  endpoint handling, receive DMA, and destination processing;
- the fanout test does not create competing sources at a shared router; and
- the 3×3 proof mixes the measured route with acknowledgment and shutdown
  traffic.

Those observations motivated a dedicated SST-only test and exposed one
important semantic boundary: Merlin reserves bandwidth for the whole packet
but calls the endpoint when its head arrives. Mittens now delays NIC
visibility until the tail crosses the destination link.

The follow-up `network-timing-validation` suite contains 13 trials and 21
packet observations:

- 1, 8, 64, and 512 words over one hop;
- 64 words over one, two, and four Manhattan hops; and
- one, two, and four simultaneous one-hop sources carrying either one or 64
  words.

For the 32-bit, 1 GHz, 10 ns-link configuration, all measured head and
completion timestamps matched:

```text
head = 35 + 12 * (hops - 1)
completion = head + words - 1
```

In the incast cases, every earlier equal-size contender added exactly
`words` cycles. The result was 21/21 exact passes with zero absolute cycle
error. The ordinary QEMU-backed pair, 3x3 route, and 1-to-24 bounded fanout
tests also passed after the endpoint correction.

## Required Work Before Epoch C

Create a dedicated component-timing suite that emits:

```text
component
configuration
predicted_cycles
measured_cycles
absolute_error
percentage_error
pass_or_fail
```

The remaining controlled cases are:

1. End-to-end QEMU/SST set, load, execute, store, and two-array-overlap analog
   trials with timestamps surrounding only the modeled device service.
2. An explicit statement that the current CPU model is an issue-throughput
   abstraction, or implementation and validation of operation-specific RVV
   latency before making detailed digital-core timing claims.

## Conclusion

The quick audit supports the following bounded statement:

> The current CPU issue-width arithmetic, analog shared-link/device timing,
> and isolated mesh serialization, multi-hop, and contention timing agree
> exactly with their declared models. Operation-specific RVV timing and
> end-to-end analog boundary timing still require controlled validation before
> the simulator/compiler configuration can be frozen as Epoch C.
