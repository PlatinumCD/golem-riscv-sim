# End-to-End Analog Timing Validation

**Date:** 2026-07-30  
**Configuration:** 9x9 arrays, one shared half-duplex 256-bit link at 1 GHz,
eight-cycle per-array compute latency, native numerical backend  
**Result:** single-array and dual-array reference timelines matched exactly

## Question

Does a custom Golem instruction decoded by QEMU and transported over fd 43
and fd 41 receive exactly the analog service time declared by the SST model?
For multiple arrays, does the model serialize transfers over one shared link
while allowing independent compute engines to overlap?

## Measured boundary

The test exercises the complete path:

```text
bare-metal RISC-V guest
  -> Golem custom instruction
  -> QEMU decode and guest-memory snapshot
  -> fd 43 command/payload
  -> fd 41 synchronization boundary
  -> SST array queue, shared link, and compute engine
  -> fd 43 status/output
  -> QEMU guest memory
```

The guest produces and checks numerical matrix-vector results. Separately,
SST trace mode records `submitted`, transfer start/finish, compute
start/finish, and completion timestamps. CPU instructions that prepare data
or reach the next command are outside each modeled device-service interval.

## Reference method

The analyzer treats the command submission timestamps at the QEMU/SST
boundary as external arrivals to an independent Python scheduler. That
scheduler implements:

- one ordered queue per array;
- eight float32 words per 256-bit link beat;
- one half-duplex link beat per tile per cycle;
- deterministic round-robin arbitration between arrays; and
- one independent eight-cycle compute engine per array.

It predicts every later phase timestamp. Acceptance requires exact equality
for the complete ordered event trace, active-cycle total, and link-beat
total. A phase that advances by `N` device cycles must also advance by
exactly `N` nanoseconds in SST.

## Results

| Case | Arrays | Commands | Predicted active cycles | Measured active cycles | Predicted link beats | Measured link beats | Cross-array transfer overlap | Cross-array compute overlap |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Single | 1 | 4 | 23 | 23 | 15 | 15 | 0 | 0 |
| Dual | 2 | 8 | 37 | 37 | 30 | 30 | 14 cycles | 2 cycles |

The single-array closed form is:

```text
set    = ceil(81 / 8) = 11 cycles
load   = ceil( 9 / 8) =  2 cycles
execute                 =  8 cycles
store  = ceil( 9 / 8) =  2 cycles
total                    = 23 active cycles
link beats               = 15
```

In the dual-array guest, command arrivals are interleaved with exact QEMU CPU
progress. The independent reference therefore predicts the observed 37-cycle
timeline from those arrival cycles rather than incorrectly assuming that all
eight commands arrive at cycle zero. During that real arrival schedule, the
two arrays have 14 overlapping transfer-demand cycles but still consume only
one beat per tile per cycle. Their execute intervals overlap for two cycles.

Both guests returned correct matrix-vector outputs and exited cleanly.

## What this establishes

This closes the first controlled analog-path timing gate:

- QEMU decodes all four exercised Golem operations;
- fd 43 carries commands, input data, completions, and output data;
- fd 41 stops and resumes the guest at the modeled boundary;
- the SST link charges exactly one cycle per 256-bit beat;
- array-local FIFO dependencies are preserved;
- transfers contend for one tile-wide link; and
- distinct array compute engines can progress concurrently.

It validates the simulator mechanism. It is not evidence that eight cycles is
the physically correct latency for a future analog array; device-calibrated
latency, energy, area, and non-ideality studies remain architectural inputs.

## Reproduction

```bash
./tests/analog-timing-validation/run-test.sh
```

The raw phase traces, per-tile summaries, logs, and joined results table are
retained under `build/tests/analog-timing-validation/`.
