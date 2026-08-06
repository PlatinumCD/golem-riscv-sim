# GPT-2 full-factorial compiler experiment

This test measures the independent and combined effects of five compiler
features. The test keeps the scheduler search depth and simulated hardware
constant.

The variable factors are:

- boundary regret;
- compact-region placement;
- spatial link pressure;
- width-two reduction balancing; and
- digital matmul distribution.

Transfer cost is not a variable factor. Sculptor always uses transfer cost as
the base Greedy score. Removing the `transfer-cost` text does not disable the
score.

The test contains this complete matrix:

```text
2^5 feature combinations x 4 token lengths = 128 deployments
```

Every deployment uses:

- GPT-2-small with 4, 8, 16, or 32 tokens;
- greedy-timing placement;
- lookahead 3 and beam width 8;
- diagonal candidate scope;
- a 12 by 12 mesh;
- four 1024 by 512 analog arrays per tile;
- dual issue at 1 GHz;
- 100 ns analog MVM latency;
- native local memory;
- asynchronous route transmission; and
- summary performance profiling.

List the 32 configurations:

```bash
./tests/sculptor-gpt2-factorial/list-configurations.sh
```

Build the selected matrix:

```bash
./tests/sculptor-gpt2-factorial/build-test.sh
```

Run the selected matrix in the foreground:

```bash
./tests/sculptor-gpt2-factorial/run-test.sh
```

Both scripts preserve completed configurations. Use these variables to select
a subset:

```bash
MITTENS_GPT2_FACTORIAL_TOKENS="4 16" \
MITTENS_GPT2_FACTORIAL_CONFIGS="br0-cr0-lp0-rb0-dm0 br1-cr1-lp1-rb1-dm1" \
    ./tests/sculptor-gpt2-factorial/build-test.sh
```

Successful simulations preserve their CSV, JSON, logs, compiler summaries,
task maps, and route manifests. They delete only reproducible core objects and
ELFs by default. Set `MITTENS_GPT2_FACTORIAL_PRUNE_SUCCESSES=0` to retain those
binary artifacts.

The output root is:

```text
build/tests/sculptor-gpt2-factorial/
```

`results.csv` contains one row for every expected trial. `analysis.md` reports
main effects, pairwise interactions, and the best configurations.
