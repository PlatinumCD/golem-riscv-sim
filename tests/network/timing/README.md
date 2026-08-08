# Isolated network timing validation

This suite validates the 32-bit, 1 GHz Platform v0.1 mesh without QEMU,
runtime, receive-DMA, or application work inside the measured interval.
SST-only probes inject packets at the same exact simulation cycle and record
both Merlin's head-flit callback and the cycle when the packet's tail has
crossed the destination link. The latter is the point at which a Mittens tile
may expose the burst to its NIC and receive-DMA path.

The suite covers:

- 1, 8, 64, and 512-word serialization over one hop;
- 64-word transfers over 1, 2, and 4 Manhattan hops; and
- 1, 2, and 4 simultaneous one-hop sources contending for one destination
  router's local output, using both 1-word and 64-word packets.

For this test's 32-bit-per-cycle links, 1 GHz clock, 10-cycle SST link
latency, and default Merlin router pipeline, the head of an uncontended
packet arrives after:

```text
head_cycles = 35 + 12 * (Manhattan_hops - 1)
packet_cycles = payload_words
completion_cycles = head_cycles + packet_cycles - 1
```

The one-hop path includes three configured 10-cycle SST links and five Merlin
endpoint/router pipeline cycles. Every additional Manhattan hop adds one
10-cycle physical link and two router pipeline cycles. For equal-size
one-hop incast packets, sorted arrivals and completions are predicted by:

```text
head[rank] = 35 + rank * packet_cycles
completion[rank] = 35 + (rank + 1) * packet_cycles - 1
```

The analyzer requires exact agreement for both head and completion timestamps,
not merely monotonic scaling.

Run:

```bash
./tests/network/timing/run-test.sh
```

The test produces raw receipts, router statistics, logs, a trial manifest,
and the joined predicted-versus-measured table under:

```text
build/tests/network-timing-validation/
```
