# Sculptor Model-Family Simulation Suite

This test adapts the non-GPT model cases from the pinned Sculptor repository.
It includes linear, convolution, GRU, LSTM, RNN, and transformer cases.

For every case, `run-test.sh CASE` does these operations:

1. Export the upstream PyTorch fixture to tensor-level MLIR.
2. Lower it through the RA-tree deployment pipeline.
3. Build one bare-metal RISC-V ELF for each active tile.
4. Run each ELF in a QEMU process inside the SST mesh.
5. Store the SST simulated completion time in `result.csv`.

The baseline uses one active tile with four analog arrays. A test can override
the mesh size, digital worker count, and SST thread count. Each tile has 64 MiB
RAM because transformer constants are part of its ELF image.

Run one case:

```bash
tests/sculptor-ra-tree-model-suite/run-test.sh gru_cell_with_bias
```

Run every non-GPT case:

```bash
tests/sculptor-ra-tree-model-suite/run-all.sh
```

The combined table is written to
`build/tests/sculptor-ra-tree-model-suite/native/simulation-runtimes.csv`.

Use the private-L1 memory-hierarchy timing backend instead:

```bash
tests/sculptor-ra-tree-model-suite/run-test.sh --memhierarchy gru_cell_with_bias
tests/sculptor-ra-tree-model-suite/run-all.sh --memhierarchy
```

Those results are written below `build/tests/sculptor-ra-tree-model-suite/memhierarchy/`.

The memHierarchy backend aggregates boot-time memory accesses into one
initialization handshake by default. Memory accesses after initialization use
the configured cache and memory timing paths.

Set `GOLEM_MODEL_MEMORY_INIT_BATCHING=false` only for a diagnostic run. This
setting sends each initialization access through SST and greatly increases
the host runtime.

All suite parameters are in `parameters.sh`. The default array size is
1024x512 for every fixture. Override either dimension when required:

```bash
GOLEM_MODEL_ARRAY_ROWS=16 GOLEM_MODEL_ARRAY_COLS=16 \
GOLEM_MODEL_CPU_ISSUE_WIDTH=2 \
  tests/sculptor-ra-tree-model-suite/run-test.sh linear_with_bias
```

The network sends each tensor as a sequence of bounded packets. The default
packet size equals the network buffer capacity. Tensor size does not change
this packet size.

Set `GOLEM_MODEL_NETWORK_PACKET_WORDS` to use a smaller packet. The value must
be at least five words and must not exceed the network buffer capacity.

The default mapping planner applies these strategies in this order:

1. `setup-first`
2. `mvm-wave`
3. `fan-out-cut`
4. `consumer-bound-fill`

Set `GOLEM_MODEL_PLANNER_STRATEGIES` to change this ordered strategy list.

Set `GOLEM_MODEL_DUPLICATE_MATRICES=1` to run
`--sculptor-duplicate-matrices` after MVM expansion and before RA-tree
construction.

Enable performance tracing, plus task traces when the runtime emits task events:

```bash
GOLEM_MODEL_PROFILE_MODE=trace \
  tests/sculptor-ra-tree-model-suite/run-test.sh linear_with_bias
```

The trace files are below that case's `trace/` output directory.

Run SST with multiple host threads by setting `GOLEM_MODEL_SST_THREADS`. The
default is one thread. The `sst.simple` partitioner cuts the graph at mesh
links while it keeps each tile's router and private memory path together.

```bash
GOLEM_MODEL_MESH_ROWS=4 \
GOLEM_MODEL_MESH_COLS=4 \
GOLEM_MODEL_DIGITAL_WORKERS=8 \
GOLEM_MODEL_BALANCE_DIGITAL_WORK=1 \
GOLEM_MODEL_SST_THREADS=8 \
  tests/sculptor-ra-tree-model-suite/run-test.sh \
    --memhierarchy transformer_block_with_bias
```

Each `result.csv` records simulated time and simulation wall time. A parallel
run is valid only when its simulated result matches the serial result.
