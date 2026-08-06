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
MITTENS_FANOUT_MODE=blocking ./tests/transmit-fanout/run-test.sh
```

Limit a diagnostic run without deleting other successful trials:

```bash
MITTENS_FANOUT_MODE=blocking \
MITTENS_FANOUTS="8 24" \
MITTENS_FANOUT_QUANTA="1000000 10000" \
./tests/transmit-fanout/run-test.sh
```

Each trial preserves its UART log, route manifest, router statistics, raw
per-tile profile, joined report, and summary under
`build/tests/transmit-fanout/results/`.
