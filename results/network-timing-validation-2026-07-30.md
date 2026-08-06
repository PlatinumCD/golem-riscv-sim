# Isolated Mesh Timing Validation

**Date:** 2026-07-30  
**Configuration:** 32-bit links, 1 GHz link clock, 10 ns SST link latency,
dimension-order mesh routing  
**Result:** 21/21 exact packet timestamp checks passed

## Question

Does the SST mesh charge exactly the declared number of cycles for packet
serialization, Manhattan distance, and shared-output contention, independent
of QEMU, runtime, DMA, and application work?

## Timing boundary correction

The first controlled run found that Merlin reserves link bandwidth for every
flit but delivers the `SimpleNetwork::Request` to its endpoint when the head
flit arrives. That is appropriate network-interface behavior, but the
original Mittens tile immediately exposed the request's complete host-side
payload vector to fd 42.

```text
before
------
head arrives ----> complete payload visible to the tile
                   link remains occupied by unseen tail flits

after
-----
head arrives ----> wait for remaining physical beats ----> payload visible
                   <---- Merlin link remains occupied ---->
```

The tile now applies:

```text
packet cycles =
    ceil(packet_words * 32 / mesh_link_width_bits)
start = max(head arrival, destination link next-available)
completion = start + packet cycles - 1
next-available = start + packet cycles
```

This does not change the guest's 32-bit NIC word ABI. It makes the time at
which software or receive DMA can consume the burst agree with physical
packet completion and prevents a later short packet from overtaking an earlier
long packet at the destination endpoint.

## Validated timing law

For the selected configuration, the measured head-flit law is:

```text
head_cycles = 35 + 12 * (Manhattan_hops - 1)
```

The one-hop path contains three 10-cycle SST links and five endpoint/router
pipeline cycles. Each additional Manhattan hop adds one 10-cycle link and two
router pipeline cycles.

At 32 bits per cycle:

```text
completion_cycles = head_cycles + payload_words - 1
```

For simultaneous equal-size one-hop incast, sorted by arrival:

```text
head[rank] = 35 + rank * payload_words
completion[rank] = 35 + (rank + 1) * payload_words - 1
```

## Results

### Serialization

| Words | Predicted head | Measured head | Predicted completion | Measured completion | Error |
|---:|---:|---:|---:|---:|---:|
| 1 | 35 | 35 | 35 | 35 | 0 |
| 8 | 35 | 35 | 42 | 42 | 0 |
| 64 | 35 | 35 | 98 | 98 | 0 |
| 512 | 35 | 35 | 546 | 546 | 0 |

### Manhattan distance

All distance trials carry 64 words.

| Hops | Predicted head | Measured head | Predicted completion | Measured completion | Error |
|---:|---:|---:|---:|---:|---:|
| 1 | 35 | 35 | 98 | 98 | 0 |
| 2 | 47 | 47 | 110 | 110 | 0 |
| 4 | 71 | 71 | 134 | 134 | 0 |

### Shared-output contention

| Words | Sources | Measured completion cycles, sorted | Expected | Error |
|---:|---:|---|---|---:|
| 1 | 1 | 35 | 35 | 0 |
| 1 | 2 | 35, 36 | 35, 36 | 0 |
| 1 | 4 | 35, 36, 37, 38 | 35, 36, 37, 38 | 0 |
| 64 | 1 | 98 | 98 | 0 |
| 64 | 2 | 98, 162 | 98, 162 | 0 |
| 64 | 4 | 98, 162, 226, 290 | 98, 162, 226, 290 | 0 |

Every source in a contention trial injected on the same exact SST cycle.

## Regression evidence

After correcting packet completion, these QEMU-backed tests also passed:

- two-tile request/acknowledgment;
- four-hop 3x3 route plus reverse acknowledgment; and
- bounded fanout from one through 24 destinations at three QEMU instruction
  quanta.

A full four-token GPT-2 Greedy-L3 analog/digital regression then exposed and
closed the variable-length ordering case. Independently delaying every tail
allowed a later short packet to complete before an earlier long packet when
Merlin reported clustered heads. Serializing completions through the physical
destination-link next-available time removed that impossible overtaking. Both
61-active-core deployments subsequently passed with unchanged numerical
signatures.

## Reproduction

```bash
./tests/network-timing-validation/run-test.sh
```

Raw receipts, router statistics, per-trial logs, the trial manifest, and the
machine-readable prediction comparison are retained under:

```text
build/tests/network-timing-validation/
```

The joined table is `results.csv`.

## Bounded conclusion

The configured SST mesh now has exact component-level evidence for physical
packet serialization, distance-dependent head latency, equal-size incast
contention, and tail-complete NIC visibility. This validates the declared
network model; it is not a claim that the selected 10 ns links or Merlin
router pipeline match a fabricated chip implementation.
