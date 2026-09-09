# Materialized workload execution test

This is a compiler-dependent integration test, not a hardware-only test.
It lowers six fixtures through Sculptor, links the generated tile objects,
and runs them in QEMU/SST with streaming memory and the epoch barrier.

Cases cover pointwise work, a fork, pooling, concatenation, reduction, and
layout conversion. Small exactly representable float32 values allow exact
output checks against [case_oracle.cpp](case_oracle.cpp).

The harness checks generated ABI descriptors, full/tail DMA requests,
all-tile completion, global-memory accounting, and final values. Input
initialization and output verification are distinguished from workload traffic.

## Run

```bash
bash tests/runtime/materialized-functional/run-test.sh
```

Compiler tools and the generated-ABI deployment test must be available.
The runner also invokes `tests/compiler/exact-ram-readiness` and
`tests/compiler/parametric-noc`, which are now local-only. Without those
directories, the complete gate cannot run.
The fixture sources live in [fixtures/](fixtures/); expectations are also
defined by [cases.json](cases.json) and the validators.

To reuse already-generated objects:

```bash
bash tests/runtime/materialized-functional/run-case.sh pointwise \
  tests/results/materialized-functional/artifacts/pointwise
```

Defaults are under `tests/results/materialized-functional/`.
`MITTENS_MATERIALIZED_ARTIFACT_ROOT` changes the generation directory;
`MITTENS_MATERIALIZED_RUN_DIR` changes the per-case execution output.
See the scripts before overriding synchronization or RAM-channel settings.
