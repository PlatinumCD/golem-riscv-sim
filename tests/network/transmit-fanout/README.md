# Transmit fanout validation

This test sends 512-word tensors from tile 0 to tile 1 over a physical 32-bit
link. It sweeps 1, 2, 4, 8, 16, and 24 routes at QEMU instruction quanta of
1,000,000, 100,000, and 10,000.

Four modes are available:

- `polling`: preserved pre-fix results for the original busy-retry behavior;
- `blocking`: one source task with the timestamped descriptor doorbell;
- `overlap-blocking`: one independent source task per route, blocking policy;
- `async`: the same independent-task graph with conservative task/transmit
  overlap enabled.

```bash
MITTENS_FANOUT_MODE=blocking ./tests/network/transmit-fanout/run-test.sh
```

Limit a diagnostic run without deleting other successful trials:

```bash
MITTENS_FANOUT_MODE=blocking \
MITTENS_FANOUTS="8 24" \
MITTENS_FANOUT_QUANTA="1000000 10000" \
./tests/network/transmit-fanout/run-test.sh
```

Each trial preserves its UART log, route manifest, router statistics, raw
per-tile profile, joined report, and summary under
`build/tests/transmit-fanout/results/`.

## Route-tail regression

The focused regression uses the GPT-2 failure conditions: one source task,
two 768-word routes, 16 network-buffer cells, RX-DMA queue depth 4, a 1,000
instruction synchronization quantum, and the private-L1 memhierarchy backend.
It requires all 775 frame words and one reassembled payload DMA burst for both routes.

```bash
./tests/network/transmit-fanout/run-tail-regression.sh
```

The contended variant launches eight simultaneous producers on a 9x1 mesh.
All 16 routes converge on the final link into tile 8.

```bash
./tests/network/transmit-fanout/run-tail-contention-regression.sh
```

The bidirectional variant maps 32 tiles onto an 8x4 mesh. Each tile sends two
768-word routes to its mirrored peer while receiving two routes in return.

```bash
./tests/network/transmit-fanout/run-tail-bidirectional-regression.sh
```

This regression uses 16 SST threads by default. Set
`MITTENS_TAIL_SST_THREADS` to test a different thread count.

The sustained regression repeats 64-word transfers for four task waves. It
checks 256 routes and 16,384 payload words. Set `MITTENS_TAIL_STRESS_WAVES`
or `MITTENS_TAIL_STRESS_WORDS` to increase the traffic scale:

```bash
./tests/network/transmit-fanout/run-tail-sustained-regression.sh
```

## Receive-order regression

The focused receive-order gate sends eight independent one-word frames from
each of two converging sources while the destination waits 250,000 guest cycles
before polling, then pauses once more immediately before its first RX DMA
submission. It verifies that at least five headers from both sources reached
the tile before the first RX DMA, stretches RX-DMA setup to force a real wait,
then requires every frame to complete without a zero-tick NIC receive wakeup.
This catches SST reporting a header as guest-visible while QEMU is correctly
hiding it behind an active DMA from the same source.

```bash
./tests/network/transmit-fanout/run-receive-order-regression.sh
```
