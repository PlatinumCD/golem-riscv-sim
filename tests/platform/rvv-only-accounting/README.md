# RVV-only instruction accounting diagnostic

This targeted diagnostic isolates vector issue accounting from scratchpad,
DMA, NoC, and ordinary memory timing. One vector register is initialized
before the task-trace start marker. The measured region then executes exactly
`K` `vadd.vx` instructions, followed by the task-trace finish marker.

The sweep is:

```text
K = 32, 64, 128, 256, 512, 1024
```

The CSV reports the exact timed vector count (`K`), scalar instructions in the
timed interval, guest CPU cycles, measured task cycles, synchronization events,
and cumulative SST instruction counters. The cumulative vector counter may
include the pre-timing vector-register initialization; the timed count is the
one used for the `delta cycles / delta K` diagnostic.

Run it with:

```sh
./tests/platform/rvv-only-accounting/run-test.sh
```

Results are written to:

```text
build/tests/platform/rvv-only-accounting/results.csv
```
