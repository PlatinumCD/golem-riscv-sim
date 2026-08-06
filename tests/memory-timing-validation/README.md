# Private-L1 timing validation

This gate validates the current blocking StandardMem/private-L1 model:

- 32 KiB capacity;
- four-way set associativity;
- 64-byte lines;
- LRU replacement;
- two-cycle L1 lookup at 1 GHz;
- one-nanosecond CPU/L1 and L1/memory links; and
- a 50-nanosecond simple lower-memory backend.

Initialization batching is enabled, so CRT and image setup do not prepopulate
the cache. Two bare-metal guests then generate controlled traffic:

1. Five addresses separated by 8 KiB map to one set and verify cold misses,
   read and write hits, four-way conflict eviction, and LRU behavior.
2. A 513-line working set crosses the 512-line cache capacity and verifies
   the resulting eviction.

For this configuration, every L1 hit must complete in exactly five cycles and
every miss in exactly 61 cycles. The analyzer checks every request timestamp,
address, direction, cache statistic, and aggregate wait total.

Run:

```bash
./tests/memory-timing-validation/run-test.sh
```
