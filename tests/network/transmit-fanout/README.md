# Transmit fan-out regression

Tile 0 sends 512-word payloads to tile 1. The runner varies route count
(1, 2, 4, 8, 16, 24) and instruction quantum (1,000,000, 100,000, 10,000)
to check transmit synchronization and backpressure.

This is a two-tile transport regression, not a four-direction bandwidth test.

## Run

```bash
MITTENS_FANOUT_MODE=blocking bash tests/network/transmit-fanout/run-test.sh
```

The script accepts `polling` (the default), `blocking`, `overlap-blocking`,
and `async`. These select existing guest binaries/task layouts; a mode name
does not restore an older simulator implementation.

For a smaller run:

```bash
MITTENS_FANOUT_MODE=blocking MITTENS_FANOUTS="8 24" \
MITTENS_FANOUT_QUANTA="1000000 10000" \
bash tests/network/transmit-fanout/run-test.sh
```

Output goes under `tests/results/transmit-fanout/results/` by default,
grouped by mode, route count, and quantum. Re-running a case replaces its
raw CSV/report files; preserve a copy before re-running a comparison.

## Focused regressions

| Script | Case |
|---|---|
| run-tail-regression.sh | Two routes with partial final bursts |
| run-tail-contention-regression.sh | Eight producers converging on one destination |
| run-tail-bidirectional-regression.sh | Mirrored peers sending and receiving |
| run-tail-sustained-regression.sh | Repeated transfer waves |
| run-receive-order-regression.sh | Delayed receive and same-source ordering |
| run-receive-head-blocking-regression.sh | Blocked receive-head ownership |
| run-software-payload-regression.sh | Software-consumed payloads |

Run scripts from the repository root using their full paths.
Configuration details live in each runner and its simulation file.
