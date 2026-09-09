# Materialized functional QEMU/SST gate

This directory is the focused M4 execution gate from
the earlier implementation contract.  It does not define or hand-author Tile ABI
tables.  Each source fixture is lowered by the production Sculptor pipeline,
and `generate-case.sh` produces the real per-tile compiler objects plus the
strict deployment manifest.  `run-case.sh` then links those objects to a
test-only platform entry point and executes them through repository QEMU,
the streaming SST tile, the shared sparse global RAM controller, and the
modeled deployment epoch barrier.

The six cases are pointwise, fork, pool, concat, reduction, and explicit
layout conversion.  Inputs and exact reference functions live independently
of the runtime entry point in `fixtures/` and `case_oracle.cpp`.  Distinct
semantic-layer identities keep the intended producer/consumer DAG visible
through the production canonicalizer.  Before object generation, the harness
requires those identities, the expected operation and epoch depths,
materialized boundaries, zero direct routes, and logical full/tail transfer
counts to survive in the generated compiler artifacts.  Values are small
exactly representable `f32` integers, so the output comparison is exact rather
than tolerance-based.  The layout-conversion case uses a 129 by 8 interleaved
view whose canonical model-output conversion includes an exact 4,096-byte
main transfer plus a 32-byte tail.

For every active tile, an ordinary-QEMU preflight and the SST guest derive the
expected DMA request and byte counts from the compiler-emitted Tile ABI v3
materialized descriptor and segment tables.  They expand iteration, repeat,
and piece counts with checked arithmetic, reject transfers larger than 4 KiB,
and require at least one exact full-page and one tail transfer per tile before
SST starts.  Runtime execution compares those counts with the deployment
profile.  `validate_run.py` independently reconciles them with each SST
scratchpad-DMA profile and the global controller's physical request/byte
statistics, including output-validation reads.  The gate also requires exact
all-tile ABI/epoch preflight, zero direct routes in the materialized cases,
one clean pass per active tile, and exact external outputs.

Run the complete gate with:

```bash
tests/runtime/materialized-functional/run-test.sh
```

The 300-second compiler-stage cutoff is enforced inside both production
compiler scripts.  A single already-generated artifact can be rerun without
compiler work using:

```bash
tests/runtime/materialized-functional/run-case.sh pointwise \
  build/tests/materialized-functional/artifacts/pointwise
```

Phase-7 host/runtime A/B runs can select independent evidence directories and
override only the two intended simulator parameters while preserving the
default 1,000-instruction quantum and one-channel RAM configuration:

```bash
MITTENS_MATERIALIZED_RUN_DIR=/absolute/evidence/directory \
MITTENS_MATERIALIZED_SYNC_INSTRUCTION_QUANTUM=1000000 \
MITTENS_MATERIALIZED_GLOBAL_RAM_CHANNELS=32 \
tests/runtime/materialized-functional/run-case.sh pointwise \
  build/tests/materialized-functional/artifacts/pointwise
```

The complete gate finishes by running the existing real generated-ABI
direct-NoC QEMU/SST preservation test.
