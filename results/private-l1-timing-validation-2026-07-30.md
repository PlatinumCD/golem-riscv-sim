# Private-L1 timing validation — 2026-07-30

## Result

PASS. Controlled conflict and capacity tests match every predicted address,
direction, hit/miss decision, response latency, cache statistic, and aggregate
wait cycle.

## Configuration

- capacity: 32 KiB
- associativity: four-way
- line size: 64 bytes
- replacement: LRU
- L1 lookup: two cycles at 1 GHz
- CPU/L1 and L1/lower-memory links: 1 ns
- lower-memory latency: 50 ns
- initialization batching: enabled, leaving the measured cache cold

For this configuration, an L1 hit is exactly five cycles and a miss is exactly
61 cycles.

## Exact observations

| Case | Accesses | Reads | Writes | Hits | Misses | Predicted wait cycles | Measured wait cycles |
|---|---:|---:|---:|---:|---:|---:|---:|
| Conflict/LRU | 10 | 9 | 1 | 4 | 6 | 386 | 386 |
| Capacity | 515 | 515 | 0 | 1 | 514 | 31,359 | 31,359 |

The conflict case maps five lines separated by 8 KiB into one set. It checks a
read hit, write hit, four-way eviction, and the exact LRU victim. The capacity
case fills all 512 lines, refreshes one line, adds line 513, and proves that
the expected older line was evicted.

Complete machine-readable output is generated at
`build/tests/memory-timing-validation/results.csv`.

Run:

```bash
./tests/memory-timing-validation/run-test.sh
```
