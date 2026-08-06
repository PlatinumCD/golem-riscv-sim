# GPT-2 8x8 test

This test is being developed in explicit stages. It downloads a pinned
revision of the official `openai-community/gpt2` base checkpoint, loads its
tokenizer and weights from local storage, and runs one deterministic eager
PyTorch inference.

The pinned model has:

- 12 transformer blocks
- 768 hidden dimensions
- 12 attention heads
- 3,072 hidden dimensions in each feed-forward network
- 1,024 available token positions
- 50,257 vocabulary entries
- `f32` weights

The test compiles a static sequence length of 128. The checkpoint vocabulary is
retained because changing it would invalidate the downloaded token-embedding
and language-model-head weights.

Run the current stage with:

```sh
./tests/sculptor-gpt2-8x8/run-test.sh
```

Downloaded model files are stored under
`build/tests/sculptor-gpt2-8x8/model` and are not committed.

The compiler probe adapts the checkpoint's 48 GPT-2 `Conv1D` projections to
mathematically equivalent rank-2 PyTorch linear operations, verifies the
adapted model against the original model, imports it through Torch-MLIR, and
then runs Sculptor layer canonicalization, extraction, and conversion. The
language-model head is the 49th linear projection.

```sh
./tests/sculptor-gpt2-8x8/run-compiler-probe.sh
```

The probe intentionally returns a nonzero status until the entire set of 49
GPT-2 projections converts to Sculptor MVM operations. Its generated IR is
stored under `build/tests/sculptor-gpt2-8x8/compiler-probe`.

`gpt2_small.py` is a separate compiler fixture with the GPT-2-small transformer
shape, deterministic synthetic weights, direct hidden-state input, and a static
sequence length of four. It can lower to 240 matrix partitions distributed
across 60 active tiles on the 8x8 mesh. Once its per-core compiler objects have
been generated, run the deployment with:

```sh
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

The deployment baseline charges 100 analog-link cycles, or 100 ns at the
1 GHz analog clock, for each MVM compute phase. Override it for an explicit
sensitivity experiment with
`MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES`.

Generate a joined task, route, packet, receive-DMA, analog, memory, and wait
profile with:

```sh
MITTENS_GPT2_PROFILE_MODE=trace \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

Raw per-tile CSVs are written under
`build/tests/sculptor-gpt2-8x8/deployment/performance-profile-raw`; joined
reports are written under the adjacent `performance-profile` directory.
`MITTENS_GPT2_PROFILE_MODE=summary` writes only unperturbed finish counters.

Generate the compact animated mesh/timeline trace with one opt-in flag:

```sh
MITTENS_VISUALIZATION_EXPORT=1 \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

This forces diagnostic trace mode, then writes
`<deployment>/visualization/trace.json`. It exports one event per task,
logical tensor route, DMA transfer, analog operation, and nonzero wait—not
individual 32-bit beats. View it with:

```sh
./visualizer/serve.sh 8000 \
  "$PWD/build/tests/sculptor-gpt2-8x8/deployment/visualization/trace.json"
```

The deployment defaults to 8x8 but accepts any positive rectangular mesh
dimensions. The compiler schedule and route manifest must use the same shape:

```sh
MITTENS_GPT2_MESH_WIDTH=12 \
MITTENS_GPT2_MESH_HEIGHT=12 \
MITTENS_VISUALIZATION_EXPORT=1 \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

This creates all 144 SST routers and tile endpoints, validates active core IDs
against that topology, and records a 12x12 visualization trace.

Attach one private L1 to every tile with:

```sh
MITTENS_MEMORY_BACKEND=memhierarchy \
MITTENS_GPT2_CPU_ISSUE_WIDTH=4 \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

This mode aggregates each tile's pre-runtime memory traffic into one explicitly
marked initialization handshake, charges it at 32 bytes per cycle plus two
setup cycles by default, and then restores detailed per-access L1 timing. The
defaults can be changed with
`MITTENS_GPT2_MEMORY_INIT_BYTES_PER_CYCLE` and
`MITTENS_GPT2_MEMORY_INIT_LATENCY_CYCLES`.

The verified sequence-length-four run completed all 60 active ELFs on the 8x8
mesh, produced 3,072 finite outputs, and reached 101.087 ms of simulated time.
Every tile reported exactly one initialization handshake. After initialization,
the active private L1s observed 14,459,504 accesses with a 96.39% hit rate.

## Analog versus digital MVM

The scheduled GPT-2 graph can also be lowered with every scheduled MVM replaced
by a digital `linalg.matmul_transpose_b`. The script verifies that all 960 MVM
executions become digital matmuls, that no analog array operations remain, and
then builds the 60 per-core objects:

```sh
./tests/sculptor-gpt2-8x8/build-digital-lowering.sh
```

Run controlled four-wide analog and digital deployments with:

```sh
MITTENS_GPT2_CPU_ISSUE_WIDTH=4 \
MITTENS_GPT2_DEPLOYMENT_DIR="$PWD/build/tests/sculptor-gpt2-8x8/deployment-analog-quad" \
./tests/sculptor-gpt2-8x8/run-deployment.sh

MITTENS_GPT2_CPU_ISSUE_WIDTH=4 \
MITTENS_GPT2_LOWERING_DIR="$PWD/build/tests/sculptor-gpt2-8x8/fixture-lowering/digital" \
MITTENS_GPT2_DEPLOYMENT_DIR="$PWD/build/tests/sculptor-gpt2-8x8/deployment-digital-quad" \
./tests/sculptor-gpt2-8x8/run-deployment.sh
```

On the historical eight-cycle, 1 GHz timing model with one shared 256-bit
analog link per tile, the verified sequence-length-four fixture completed in
38.2298 ms with native analog MVMs and 103.15 ms with digital vector MVMs.
Both runs produced 3,072 finite outputs and moved the same 773,930 network
words. The analog result was 2.698x faster in that historical configuration;
it is not a result for the new 100 ns baseline.

The superseded model with one 256-bit link per array completed the analog run
in 38.1315 ms. Sharing the link therefore added 98.3 us, or 0.258%, while
preserving the exact output signature.

The controlled token-length, scheduler, heuristic, reduction-balancing, and
analog-versus-digital matrix is a separate test:

```sh
./tests/sculptor-gpt2-scheduling-sweep/list-configurations.sh
./tests/sculptor-gpt2-scheduling-sweep/build-test.sh
./tests/sculptor-gpt2-scheduling-sweep/run-test.sh
```

See
[`../sculptor-gpt2-scheduling-sweep/README.md`](../sculptor-gpt2-scheduling-sweep/README.md)
for the complete 112-deployment contract and subset controls.
