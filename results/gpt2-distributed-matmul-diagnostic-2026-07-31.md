# GPT-2 distributed-matmul diagnostic

Date: 2026-07-31

## Result

For the four-token GPT-2 workload, the current distributed-digital-matmul
lowering is slower than leaving the attention matmuls undistributed:

| Configuration | Measured time | Relative to control |
|---|---:|---:|
| No digital-matmul distribution | 12.109068 ms | baseline |
| Distributed digital matmul | 14.600740 ms | 20.58% slower |

The control is 17.07% faster than the distributed case. Both simulations
completed successfully on all active tiles.

The regression is not caused by global mesh saturation. Distribution reduces
directional word-hops and aggregate network transit time, but introduces
expensive scalar partitioning work. The critical-path trace attributes
approximately 92.9% of the measured runtime increase to additional task
execution time rather than additional waiting.

## Controlled experiment

The two configurations use the same:

- GPT-2-small fixture and sequence length of four tokens;
- 12 by 12 mesh;
- four 1024 by 512 analog arrays per tile;
- 1 GHz tile clock and dual scalar issue;
- 100-cycle analog MVM compute latency;
- 256-bit shared tile-to-analog link;
- 32-bit, one-word-per-cycle mesh links with XY routing;
- native untimed local-memory backend;
- asynchronous route transmission;
- reduction balancing with width two;
- greedy-timing scheduler with lookahead 3, beam width 8, diagonal scope,
  transfer cost, boundary regret, and spatial link pressure;
- non-compact placement; and
- Sculptor revision `18df015c8d8179192471ad5428d82cde2d0177b2`.

The only graph-level independent variable is
`--sculptor-distribute-digital-matmul`.

Summary mode supplies the reported performance result. Trace mode supplies the
task and critical-path attribution. Trace perturbation was small:

| Configuration | Summary | Trace | Trace perturbation |
|---|---:|---:|---:|
| No distribution | 12.109068 ms | 12.105485 ms | -0.030% |
| Distributed | 14.600740 ms | 14.611015 ms | +0.070% |

## Structural and simulator measurements

| Metric | No distribution | Distributed | Change |
|---|---:|---:|---:|
| Active tiles | 66 | 71 | +5 |
| Executed tasks | 1,837 | 1,885 | +48 |
| Routes | 1,424 | 1,528 | +104 |
| Packets | 2,848 | 3,056 | +7.30% |
| Injected 32-bit words | 977,360 | 981,304 | +0.40% |
| Directional word-hops | 2,517,780 | 2,133,467 | -15.26% |
| Aggregate network transit | 4.299167 ms | 3.826574 ms | -10.99% |
| Router stalls | 123,612 | 103,010 | -16.67% |
| Retired instructions | 54,039,334 | 59,903,274 | +10.85% |
| Vector instructions | 8,724,499 | 8,748,184 | +0.27% |
| CPU cycles summed across tiles | 27,021,931 | 29,954,031 | +10.85% |
| Analog operations | 3,120 | 3,120 | unchanged |
| Analog link beats | 15,912,960 | 15,912,960 | unchanged |

More than 99% of the incremental retired instructions are non-vector
instructions. The maximum observed physical-link utilization is only about
0.25% in both runs. The mesh is therefore not globally bandwidth-saturated in
this experiment, even though short-lived local bursts still produce router
stalls.

## Critical-path diagnosis

| Critical-chain quantity | No distribution | Distributed | Change |
|---|---:|---:|---:|
| Root-to-final-task span | 11.855062 ms | 14.431331 ms | +2.576269 ms |
| Sum of task durations | 10.025333 ms | 12.339800 ms | +2.314467 ms |
| Sum of recorded waits | 1.389280 ms | 1.460112 ms | +0.070832 ms |
| Route-selected predecessors | 139 | 150 | +11 |

The direct attention-matmul replacement explains the task-time increase:

| Task group | All-task duration | Retired instructions |
|---|---:|---:|
| Control: attention scores + apply + head recombine | 0.122880 ms | 245,736 |
| Distributed: partition + shards + assembly | 2.508468 ms | 5,016,876 |
| Difference | +2.385588 ms | +4,771,140 |

The distributed `digital.matmul_partition` tasks alone consume 2.358372 ms and
retire 4,716,696 instructions. Of that, 2.293078 ms lies on the critical chain.
This one task kind explains almost the entire increase in traced task
instructions and most of the end-to-end slowdown.

The common costs that remain after this regression is fixed are also clear:

- `mixed.fused`: about 6.3 ms on either critical chain;
- `digital.qkv_split`: 2.289912 ms on either critical chain;
- `digital.layer_norm`: 0.727950 ms on either critical chain; and
- all MVM tasks together: roughly 0.14 to 0.15 ms of critical-chain task time.

At four tokens, the attention matmuls are too small to repay eager tensor
partitioning. The distribution pass improves placement and route distance, but
spends substantially more CPU time preparing the shards than the undistributed
attention computation originally required.

## Compiler prediction

| Configuration | Compiler prediction | Simulator result | Underprediction |
|---|---:|---:|---:|
| No distribution | 11.438080 ms | 12.109068 ms | 5.54% |
| Distributed | 12.331770 ms | 14.600740 ms | 15.54% |

The compiler predicts the correct ordering, but underestimates the distributed
case much more severely. Its cost model must charge the actual scalar copying
performed by partition tasks. A transfer-distance improvement must not be
allowed to hide local materialization cost.

## Correctness status

Both summary runs reported 3,072 finite output elements and every active tile
exited successfully. The output signatures are not bit-identical:

| Configuration | First element bits | Checksum bits |
|---|---:|---:|
| No distribution, summary | 3218958928 | 3164897280 |
| Distributed, summary | 3218958840 | 3158913280 |

Different floating-point reduction orders may account for the difference, but
the current harness checks finiteness rather than error against a PyTorch
reference. These runs establish execution completion, not full numerical
equivalence. A reference-tolerance check is required before using distributed
matmul in a numerical-results claim.

## Required compiler work and acceptance tests

The next implementation should eliminate eager partition copies:

1. Make contiguous shards descriptor-only views, with no elementwise copy.
2. For non-contiguous shards, either produce a shard-friendly layout upstream
   or route the needed segments directly through DMA without materializing a
   full temporary partition tensor.
3. Fuse QKV splitting with the shard-friendly layout where that avoids a
   second full-tensor traversal.
4. Vectorize any unavoidable packing, splitting, and recombination loops.
5. Charge residual packing and DMA work explicitly in the scheduling cost
   model.

The zero-copy implementation passes acceptance only if:

- `digital.matmul_partition` disappears as an executed CPU-copy task, or its
  retired instructions fall by at least an order of magnitude;
- the distributed and control outputs agree with the same PyTorch reference
  tolerance;
- trace mode shows the expected partition-copy reduction;
- summary mode shows no regression at four tokens; and
- a 4, 8, 16, and 32-token sweep identifies the size at which distribution
  becomes profitable.

After zero-copy partitioning, vectorizing `digital.qkv_split` and examining the
large common `mixed.fused` tasks are the next two performance targets. Removing
partition overhead alone is expected to bring the distributed run close to
the control, but the four-token shards may still be too small to create a
speedup.

## Run matrix status

The complete current-compiler matrix was executed:

| Run | Status |
|---|---|
| Distributed, summary | PASS |
| Distributed, trace | PASS |
| No distribution, summary | PASS |
| No distribution, trace | PASS |

The planned post-zero-copy summary/trace pair and post-vectorization
summary/trace pair were not mislabeled as completed experiments: the installed
compiler exposes the distribution pass but does not yet contain those two
transformations. The acceptance criteria above define those four follow-up
runs once their implementations exist.

## Artifacts

- Distributed experiment:
  `build/research/gpt2-digital-distribution-12x12-reduction-link-pressure-no-compact-18df015-run1`
- No-distribution experiment:
  `build/research/gpt2-no-digital-distribution-12x12-reduction-link-pressure-no-compact-18df015-run1`
- Summary data: `deployment/performance-profile/summary.json`
- Trace analysis: `deployment-trace/performance-profile/`
- Critical chains: `deployment-trace/performance-profile/critical-path.csv`
- Per-task measurements: `deployment-trace/performance-profile/tasks.csv`
- Route measurements: `deployment-trace/performance-profile/routes.csv`
