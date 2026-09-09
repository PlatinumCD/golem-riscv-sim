# CPU delivery and scratchpad deadlines

This regression checks that an unrelated memory response cannot end an
instruction delay or scratchpad access before its scheduled completion.

Run from the repository root after building hardware:

```bash
python3 -B src/sst/tests/cpu_memory_deadline.py
```

It builds two small guest programs and runs each at 500 MHz, 1 GHz, and 2 GHz,
with store-buffer depths of one and two. Ordinary memory uses noncacheable
StandardMem with 100 ns latency; the fixture has no NIC or analog operations.

| Case | Operation | Required behavior |
|---|---|---|
| CPU delivery | Buffered store, 4,096 register additions, fence, finish marker | The finish marker cannot precede the instruction budget |
| SPM service | Buffered store followed by two SPM loads | Each load receives its full 10,000-CPU-cycle service interval |

The overlapping cases require a memory response during the delay under test.
A correct payload alone does not pass: the recorded completion ticks must also
satisfy `deadline = start_tick + cycles × ticks_per_CPU_cycle`.

Each run records commands, guest and simulator hashes, traces, and an
`oracle.json` for each case. Exit codes are 0 for pass, 1 for a failed timing
check, and 2 for invalid evidence.

Use `--cpu 1GHz` for a smaller run. Optional `--reference-install PATH` compares
another installation; `--reference-only` checks that installation alone.
The host controller tests also cover interrupted delays and stale wakeups.
