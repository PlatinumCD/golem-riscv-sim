# Wormhole network timing regression

SST-only probes check the same router and NIC used by managed tiles, without
CPU, scratchpad or receive-DMA work in the measured interval.

The sweep covers 1/8/64/512-word packets over one hop, 64-word packets over
1/2/4 hops, and 1/2/4 simultaneous sources sharing a destination's local output.
Links are 32 bits at 1 GHz, with 10-cycle propagation, three-cycle router
head pipelines, and 4096-flit buffers to isolate serialization from credit stalls.

For H Manhattan hops and W payload words, the prediction in link cycles is:

```text
uncontended completion = 36 + 13*(H - 1) + W - 1
one-hop incast completion[rank] = 36 + (rank + 1)*W - 1
```

One hop crosses three links and two routers. Each extra hop adds one link
and one router. Wormhole forwarding pipelines the remaining words; competing
packets serialize on the destination output. Rank is zero-based arrival order.

The NIC callback marks **tail delivery**, not first-flit arrival. Both recorded
delivery and completion must match the prediction exactly. Router credit and
arbitration stalls are separate resource counters, not additional elapsed time.

Run `bash tests/network/timing/run-test.sh`. Receipts, counters, the trial
manifest and predicted-versus-measured table go to
`tests/results/network-timing-validation/`, or the suite runner's isolated case.
