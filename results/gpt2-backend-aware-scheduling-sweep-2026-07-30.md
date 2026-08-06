# GPT-2 Backend-Aware Scheduling Sweep

**Date:** 2026-07-30  
**Experiment size:** 112 deployments  
**Status:** 112 passed, 0 failed

## Experiment

This study evaluates GPT-2 across:

- Four token counts: 4, 8, 16, and 32.
- Fourteen scheduler configurations.
- Analog and digital compute backends.
- An 8×8 mesh containing 64 tiles.
- Four 1024×512 analog arrays per tile.
- A 1 GHz, dual-issue RISC-V CPU model.

The backend-aware timing scheduler uses an analog MVM cost model when
constructing analog deployments and a digital MVM cost model when constructing
digital deployments.

## Headline Result

The central result is not simply that analog execution is faster. Analog and
digital execution favor fundamentally different task placements.

- `greedy-timing-l3` is the fastest analog scheduler at every token count.
- `random-seed0` is the fastest digital scheduler at every token count.
- Comparing the best analog deployment with the best digital deployment
  produces a consistent system-level speedup of approximately 3.6×.
- For a fixed scheduler configuration, digital-to-analog speedup ranges from
  2.79× to 6.79×, with a median of 5.49×.
- At 32 tokens, 95.8% of task-to-core assignments differ between the analog
  and digital `greedy-timing-l3` deployments.

This supports the following architectural claim:

> Compute technology changes the optimal balance between communication
> locality and graph parallelism, so analog acceleration requires
> backend-aware scheduling.

## Best Runtime by Backend

| Tokens | Best analog configuration | Analog runtime | Best digital configuration | Digital runtime | Best-to-best speedup |
|---:|---|---:|---|---:|---:|
| 4 | `greedy-timing-l3` | 19.151 ms | `random-seed0` | 69.245 ms | 3.62× |
| 8 | `greedy-timing-l3` | 35.112 ms | `random-seed0` | 129.214 ms | 3.68× |
| 16 | `greedy-timing-l3` | 68.610 ms | `random-seed0` | 251.459 ms | 3.67× |
| 32 | `greedy-timing-l3` | 142.418 ms | `random-seed0` | 505.828 ms | 3.55× |

## Scheduler Sensitivity

Scheduler selection affects the digital backend considerably more than the
analog backend.

| Tokens | Analog best-to-worst spread | Digital best-to-worst spread |
|---:|---:|---:|
| 4 | 1.30× | 1.82× |
| 8 | 1.22× | 1.95× |
| 16 | 1.16× | 2.01× |
| 32 | 1.11× | 2.02× |

The analog spread becomes narrower as the token count increases. The digital
spread grows and reaches approximately 2× at 16 and 32 tokens.

## Why Digital and Analog Behave Differently

The 32-token digital results demonstrate that minimizing communication volume
alone is not sufficient:

| Digital scheduler | Runtime | Network traffic |
|---|---:|---:|
| `random-seed0` | 505.828 ms | 9.766 million words |
| `snake` | 1023.510 ms | 6.190 million words |

The snake placement communicates approximately 37% fewer words but takes twice
as long. It sacrifices graph parallelism and increases dependency stretch.
Across the digital schedules, dependency stretch has an average Pearson
correlation of approximately `r = 0.96` with simulated runtime.

Digital execution therefore benefits from exposing parallel compute, even when
doing so introduces additional network traffic.

The 32-token analog results show the opposite scheduling pressure:

| Analog scheduler | Runtime | Network traffic |
|---|---:|---:|
| `greedy-timing-l3` | 142.418 ms | 6.774 million words |
| `random-seed0` | 158.107 ms | 9.766 million words |

Once MVM computation becomes inexpensive, reducing communication and placing
tasks according to the analog cost model improves total runtime. Across analog
schedules, network traffic correlates positively with runtime, with an average
Pearson correlation of approximately `r = 0.63`.

## Interpretation

These results support a hardware/compiler co-design argument:

1. A scheduler optimized for expensive digital MVM work should preserve
   parallelism, even at the cost of additional communication.
2. A scheduler optimized for accelerated analog MVM work should place greater
   emphasis on locality and communication.
3. A single backend-independent placement policy cannot represent both systems
   accurately.
4. Compiler scheduling is part of the architectural performance result rather
   than a separable implementation detail.

The result also exposes an opportunity in the digital scheduler. The
digital-aware greedy timing configurations improve over several ordinary
greedy configurations, but they do not beat `random-seed0`. This suggests that
the current placement objective still does not fully capture critical-path
parallelism or dependency stretch.

## Validity and Required Follow-Up

All 112 deployments completed, produced the required number of output
elements, and contained only finite values. No register-allocation fallback
retries occurred.

Two issues must be resolved before treating the conclusions as final:

1. **Random placement needs multiple seeds.** The experiment currently contains
   only `random-seed0`. Several independent random placements are required to
   determine whether its digital result is representative or unusually good.
2. **Numerical equivalence needs stronger validation.** Scheduler
   configurations produce slightly different output bit patterns and
   checksums. This may result from floating-point reduction ordering, but the
   full outputs must be compared against the PyTorch reference using explicit
   absolute and relative error metrics.

The best-to-best 3.6× result is a co-optimized system comparison. It should not
be described as an isolated analog compute-unit speedup. A complete evaluation
should report both:

- Co-optimized analog and digital deployments.
- Iso-placement analog and digital controls.

## Artifacts

- Results table:
  `build/tests/sculptor-gpt2-scheduling-sweep/results.csv`
- Interactive dashboard:
  `build/tests/sculptor-gpt2-scheduling-sweep/dashboard/index.html`
- Complete run log:
  `build/tests/sculptor-gpt2-scheduling-sweep/full-run.log`
- Preserved shared-placement baseline:
  `build/tests/sculptor-gpt2-scheduling-sweep-shared-placement-baseline-20260729/`
