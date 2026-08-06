# GPT-2 scheduling sweep

This experiment compares Sculptor placement policies on the same
GPT-2-small-shaped compiler fixture and the same simulated hardware. It is a
controlled Cartesian product:

```text
4 static token lengths x 14 placements x 2 compute modes = 112 deployments
```

Every deployment uses:

- static sequence length 4, 8, 16, or 32;
- the 12-layer, hidden-size-768 GPT-2 fixture;
- an 8x8 mesh;
- four 1024x512 analog arrays per tile;
- a 32-bit, 1 GHz mesh link;
- a 1 GHz dual-issue scalar CPU model;
- a 100 ns analog MVM latency in both Sculptor placement costs and SST
  execution;
- native QEMU memory; and
- fixed random seed 0.

The latency may be overridden as a paired sensitivity experiment with
`MITTENS_GPT2_SWEEP_ANALOG_MVM_LATENCY_NS` and
`MITTENS_GPT2_SWEEP_ANALOG_COMPUTE_LATENCY_CYCLES`. At the default 1 GHz
analog clock, the two numeric values must match to keep compiler placement and
SST execution aligned.

The two compute modes are scheduled independently from the same unscheduled
task graph. The analog branch uses `mvm-cost-mode=analog`; the digital branch
uses `mvm-cost-mode=digital`. `analog` then uses the custom Golem analog
instructions and native analog backend. `digital` applies
`sculptor-lower-scheduled-mvm-to-digital` after placement and executes the MVM
work as RISC-V digital matmuls.

```text
                         +-> analog timing -> analog schedule -> analog ELF
common unscheduled graph |
                         +-> digital timing -> digital schedule -> digital ELF
```

This makes timing-aware placement backend-aware: `greedy-timing` receives costs
that describe the computation the selected backend will actually execute.
Random, snake, and ordinary greedy placement do not consume timing costs, so
their recorded placement cost mode is correctly `n/a`; they remain backend
controls generated from separately timed branches. The scheduled module,
task-to-core map, scheduler summary, and deployment routes are retained for
every trial.

## Placements

Run `./tests/sculptor-gpt2-scheduling-sweep/list-configurations.sh` to print
the authoritative manifest. The 14 rows are:

| Configuration | Placement |
| --- | --- |
| `random-seed0` | Random, seed 0 |
| `snake` | Serpentine mesh order |
| `greedy-l2` | Greedy, lookahead 2, beam 1 |
| `greedy-l3` | Greedy, lookahead 3, beam 1 |
| `greedy-b8` | Greedy, beam 8 |
| `greedy-timing-l2` | Timing-aware Greedy, lookahead 2, beam 1 |
| `greedy-timing-l3` | Timing-aware Greedy, lookahead 3, beam 1 |
| `greedy-timing-b8` | Timing-aware Greedy, beam 8 |
| `greedy-b8-link-pressure` | Greedy beam 8 with directed-link pressure |
| `greedy-b8-balanced-reductions` | Greedy beam 8 after width-2 reduction balancing |
| `greedy-b8-link-pressure-balanced-reductions` | Both Greedy additions |
| `greedy-timing-b8-link-pressure` | Timing-aware Greedy beam 8 with directed-link pressure |
| `greedy-timing-b8-balanced-reductions` | Timing-aware Greedy beam 8 after width-2 reduction balancing |
| `greedy-timing-b8-link-pressure-balanced-reductions` | Both timing-aware Greedy additions |

Lookahead-2 and lookahead-3 deliberately use `beam=1`. Sculptor uses beam
search, rather than recursive lookahead, when `beam` is greater than one.
Calling beam-8 configurations “L2” or “L3” would therefore mislabel duplicate
experiments.

Reduction balancing is an explicit pre-placement graph transformation:

```text
assemble task graph
  -> balance marked reductions (width 2, require a real change)
  -> build placement islands
  -> timing analysis
  -> schedule
```

Link pressure is an independent Greedy candidate-scoring term. It penalizes
candidate placements whose byte-heavy transfers share the same directed XY
mesh links.

## Build and run

Build compiler objects for all 112 deployments:

```bash
./tests/sculptor-gpt2-scheduling-sweep/build-test.sh
```

Then run every simulation in the foreground:

```bash
./tests/sculptor-gpt2-scheduling-sweep/run-test.sh
```

Both phases print an overall matrix counter before each deployment:

```text
[SIM 37/112] tokens=8 config=greedy-timing-b8 mode=analog
Passed: 36 | Failed: 0 | Remaining: 76 | Elapsed: 01:42:18
```

The build phase uses the same format with `BUILD` and `Complete`. Counts honor
all token, configuration, and compute-mode filters. Resumed runs count existing
pass/build markers, while forced runs count only work completed by the current
invocation. These are ordinary foreground log lines, so compiler, QEMU, and
SST diagnostics remain visible and are not hidden behind a terminal animation.

Both scripts are resumable. Completed builds and passing simulations are
skipped. Force either phase with:

```bash
MITTENS_GPT2_SWEEP_FORCE_BUILD=1 \
    ./tests/sculptor-gpt2-scheduling-sweep/build-test.sh

MITTENS_GPT2_SWEEP_FORCE_RUN=1 \
    ./tests/sculptor-gpt2-scheduling-sweep/run-test.sh
```

Subsets use space-separated environment selections:

```bash
MITTENS_GPT2_SWEEP_TOKENS="4 8" \
MITTENS_GPT2_SWEEP_CONFIGS="snake greedy-timing-b8-link-pressure" \
MITTENS_GPT2_SWEEP_MODES="analog digital" \
    ./tests/sculptor-gpt2-scheduling-sweep/build-test.sh
```

Use the same selections for `run-test.sh`. Core-object compilation uses every
online host CPU by default; set `MITTENS_GPT2_SWEEP_BUILD_JOBS` to cap it.
SST runs with one host thread by default for a controlled comparison; set
`MITTENS_GPT2_SWEEP_SST_THREADS` explicitly to change that.

The sweep invokes LLVM with the `basic` register allocator directly. This is a
deliberate experiment-wide setting: it avoids attempting the default allocator
and printing a failure before retrying every large generated core. No
register-allocation fallback is enabled.

The build removes multi-gigabyte transient per-core MLIR and LLVM IR after
each object succeeds. Set `MITTENS_GPT2_SWEEP_KEEP_IR=1` to retain it.

## Results

Artifacts are isolated under:

```text
build/tests/sculptor-gpt2-scheduling-sweep/
  tokens-<N>/
    common/08-task-graph.mlir
    configurations/<name>/
      analog/
        10-scheduled.mlir
        scheduler-summary.csv
        scheduling-metadata.txt
        task-core-map.csv
        deployment-routes.csv
        active-cores.txt
        cores/core-<N>.o
        deployment/simulation.log
      digital/
        10-scheduled.mlir
        scheduler-summary.csv
        scheduling-metadata.txt
        task-core-map.csv
        deployment-routes.csv
        active-cores.txt
        cores/core-<N>.o
        deployment/simulation.log
  results.csv
```

`summarize-results.sh` always emits all 112 expected rows, including
`not-built`, `built`, `failed`, and `pass` states. Each passing row includes
the placement cost mode, digital timing parameters, provenance paths, Sculptor
graph metrics, SST simulated time, output signature, active-core count,
retired scalar and vector instruction totals, modeled CPU cycles, transmitted
32-bit words, and analog-active cycles. This is simulated time, not host
wall-clock time.

The original 112-result experiment, in which analog and digital shared one
placement, is retained as:

```text
build/tests/sculptor-gpt2-scheduling-sweep-shared-placement-baseline-20260729/
```

## Interactive results dashboard

Generate a self-contained dashboard from the current `results.csv`:

```bash
./tests/sculptor-gpt2-scheduling-sweep/build-dashboard.sh
```

The generated site is written to:

```text
build/tests/sculptor-gpt2-scheduling-sweep/dashboard/
```

Serve it from the repository root:

```bash
python3 -m http.server 8000
```

Then open:

```text
http://127.0.0.1:8000/build/tests/sculptor-gpt2-scheduling-sweep/dashboard/
```

The dashboard is a 16-figure research narrative covering normalized scheduler
regret, analog speedup, token scaling, backend rank reversal, compiler-score
fidelity, instruction and communication costs, compute/communication Pareto
placement, active cores, per-tile imbalance, dependency stretch, analog
initialization amortization, scheduler feature ablations, and search strategy.
Analog results are blue, digital results are orange, and every scheduler has a
distinct marker.

The data generator reads both `results.csv` and every trial's
`MITTENS_PROFILE` records. It derives maximum and mean tile work, tile-cycle
coefficient of variation, load-balance efficiency, dependency stretch, and
analog operation totals without rerunning a simulation. Scheduler and
compute-mode controls update all figures together.
