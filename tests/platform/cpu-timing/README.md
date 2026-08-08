# CPU timing validation

This gate validates the declared Mittens CPU throughput abstraction, not a
detailed out-of-order pipeline. One bare-metal guest contains:

- an empty marker-to-marker baseline;
- exactly 1,024 scalar `addi` instructions; and
- exactly 1,027 RVV instructions, including setup and one vector store.

The guest runs at issue widths 1, 2, and 4 with 37- and 1,000-instruction
QEMU synchronization quanta. Every marked interval must satisfy:

```text
cycles = max(ceil(retired instructions / issue width),
             retired vector instructions)
```

The two quanta must produce identical instruction counts, task timestamps,
CPU cycles, and completion time for each issue width. This verifies that the
host synchronization quantum does not become an architectural timing
parameter.

Run:

```bash
./tests/platform/cpu-timing/run-test.sh
```
