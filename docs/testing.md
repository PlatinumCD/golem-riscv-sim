# Testing

Architecture performance experiments live separately in
[`studies/`](../studies/README.md). This includes the scratchpad and former
microbenchmark trees. The correctness test runners do not launch those sweeps.

Each test owns its source files and runs in the foreground. The source tree
groups tests by the subsystem that owns the primary behavior.

## Test groups

| Directory | Scope |
|---|---|
| `tests/platform/` | Bare-metal boot, RISC-V vectors, and CPU timing |
| `tests/compiler/` | Torch-MLIR, PyTorch lowering, Sculptor, and compiler integration |
| `tests/runtime/` | Runtime library and routed deployment behavior |
| `tests/memory/` | Global RAM, exact readiness, scratchpad DMA, and DMA macro contention |
| `tests/network/` | Mesh routing, pipelines, fanout, and network timing |
| `tests/analog/` | Analog instructions, arrays, routes, and distributed MVM behavior |
| `tests/models/` | Complete compiler-to-simulator model deployments |
| `tests/validation/` | Cross-component baselines and profile analysis |
| `tests/support/` | Shared shell and Python test infrastructure |

Execution cost does not determine the directory. A change in execution time
does not change the owning subsystem.

Component-owned tests remain beside their components:

```text
src/sst/tests/
```

For example, `python3 -B src/sst/tests/run-injection-order-test.py` checks the
real NIC's same-destination lane ordering and deferred-credit wakeup with no
guest runtime dependency. It is also part of the component group.

## Run tests

Run one test from its directory:

```bash
./tests/network/pair/run-test.sh
```

Run one subsystem group:

```bash
./tests/run-group.sh network
```

Group runs use the same runner, per-case timeout and saved evidence as full-suite
runs. For example, `./tests/run-group.sh network --list` lists the hardware
allowlist; `--case network/mesh-3x3 --timeout 600` narrows it. Compiler, models,
and the full validation group remain explicit opt-ins.

Run the authoritative hardware correctness gate:

```bash
bash tests/run-all.sh --suite hardware
bash tests/run-all.sh --list
bash tests/run-all.sh --case network/mesh-3x3 --timeout 600
```

No arguments selects hardware. The explicit registry covers 33 groups: host
verification, configuration, components, fixed TX/RX controller regressions, platform, memory, network, hardware
runtime, analog and performance-profile validation. It excludes compiler/model
workloads, `materialized-functional`, model-specific validation and study sweeps.
The TX controller group reuses the existing fan-out guest for eight fixed
lane/link/bank/FIFO fixtures; it does not run a characterization sweep. The RX
group runs software-payload, receive-order and receive-head-blocking regressions.
The `platform/rvv-memory-accounting` group runs three exact assembly kernels
across seven instruction-grant sizes, covering vector memory, fences and
compressed loads/stores. It requires exact vector counts and grant-invariant
instruction accounting.
The `network/communication-envelope` group runs nine fixed RX/sharing/client/
bank-phase/descriptor/backpressure regressions, not the full characterization
sweep. Its required RVV and repeated bank-phase controls protect the repaired
CPU accounting and injection-lane ordering boundaries. See the
[communication envelope](../studies/compute-communication/communication-envelope/README.md)
for the complete matrix, measurement contract and generated report.
`--suite compiler`, `--suite models` and `--suite all` are explicit opt-ins.
`--case` is repeatable and must belong to the selected suite. The default
per-case deadline is 300 seconds. Independent cases continue after failures.

Each invocation creates a unique `build/src/test-runs/<id>` directory (under the
selected build root when overridden), with private guest/runtime artifacts,
commands, logs, loaded-binary provenance, source and guest hashes, resolved
configuration references and JSON/CSV validation results. Required missing
dependencies/artifacts fail the gate; timeouts remain `TIMEOUT`, not generic
failures. Skipped cases never substitute for required successful coverage.

The actual simulator measurement fixture must validate, not just the validator's
unit fixtures. Historical comparison is separate and requires
`python3 tools/hardware/regression.py --reference-install /pinned/baseline/install`.
Historical reference schemas remain unmodified and explicitly unvalidated by
the current checker; the candidate must pass current measurement validation.

The shared `tests/support/test-env.sh` file supplies repository, build, and
installation paths. Direct tests write under `build/src/tests/`; the authoritative
runner assigns unique per-case roots to prevent stale outputs from passing.

## Compiler and runtime proof

```bash
./tests/compiler/torch-mlir/run-test.sh
./tests/compiler/pytorch-single-core/run-test.sh
./tests/compiler/sculptor-ra-tree-single-tile/run-test.sh
./tests/runtime/library/run-test.sh
```

The single-tile Sculptor test exports a deterministic PyTorch linear layer.
It builds, maps, places, and outlines the RA tree. Then it emits a RISC-V
object with the generated tile ABI.

The shared RA-tree entry points are:

```text
build-scripts/lower-sculptor-ra-tree.sh
build-scripts/build-sculptor-core-objects.sh
```

See [Sculptor RA-tree migration](sculptor-ra-tree-migration.md) for the
compiler, runtime, QEMU, and SST boundary.

## Model-family simulation

`tests/models/sculptor-ra-tree/` adapts all 25 non-GPT Python fixtures from
the pinned Sculptor repository. Each case produces a bare-metal tile ELF.
QEMU and SST run the deployment and record the simulated completion time.

Run all model cases with the native memory backend:

```bash
./tests/models/sculptor-ra-tree/run-all.sh
```

Run all model cases with the memHierarchy backend:

```bash
./tests/models/sculptor-ra-tree/run-all.sh --memhierarchy
```
