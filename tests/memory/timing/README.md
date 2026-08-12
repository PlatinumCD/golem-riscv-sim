# Private-L1 timing validation

This gate validates the StandardMem private-L1 model:

- 32 KiB capacity;
- four-way set associativity;
- 64-byte lines;
- LRU replacement;
- two-cycle L1 lookup at 1 GHz;
- one-nanosecond CPU/L1 and L1/memory links; and
- a 50-nanosecond simple lower-memory backend.

Initialization batching is enabled. CRT and image setup do not prepopulate the
cache. Four bare-metal guests then generate controlled traffic:

1. Five addresses separated by 8 KiB map to one set and verify cold misses,
   read and write hits, four-way conflict eviction, and LRU behavior.
2. A 513-line working set crosses the 512-line cache capacity and verifies
   the resulting eviction.
3. A store stream fills an eight-entry store buffer and verifies readback.
4. One RVV load crosses a cache-line boundary and issues both line requests
   at the same simulation tick.
5. Two independent scalar loads issue together through the load queue.
6. A two-load pointer chain waits for the first load before it issues the
   second load.

For this configuration, every L1 hit must complete in exactly five cycles and
every miss in exactly 61 cycles. The analyzer checks every request timestamp,
address, direction, cache statistic, and aggregate wait total.

Use this command to examine the batched fd 41 transport:

```bash
MITTENS_MEMORY_ACCESS_BATCHING=1 ./tests/memory/timing/run-test.sh
```

The cache tests check exact latency. The buffered-store test checks overlap
and functional ordering. The vector test checks grouped concurrent reads. The
scalar dependency tests compare safe overlap against a pointer chain.

Run:

```bash
./tests/memory/timing/run-test.sh
```
