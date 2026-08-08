# Testing

Each test owns its source files and runs in the foreground. The source tree
groups tests by the subsystem that owns the primary behavior.

## Test groups

| Directory | Scope |
|---|---|
| `tests/platform/` | Bare-metal boot, RISC-V vectors, and CPU timing |
| `tests/compiler/` | Torch-MLIR, PyTorch lowering, Sculptor, and compiler integration |
| `tests/runtime/` | Runtime library and routed deployment behavior |
| `tests/memory/` | Private caches, shared caches, scratchpad DMA, and memory timing |
| `tests/network/` | Mesh routing, pipelines, fanout, and network timing |
| `tests/analog/` | Analog instructions, arrays, routes, and distributed MVM behavior |
| `tests/models/` | Complete compiler-to-simulator model deployments |
| `tests/validation/` | Cross-component baselines and profile analysis |
| `tests/support/` | Shared shell and Python test infrastructure |

Execution cost does not determine the directory. A change in execution time
does not change the owning subsystem.

Component-owned tests remain beside their components:

```text
components/elements/mittens/tests/
visualizer/tests/
```

## Run tests

Run one test from its directory:

```bash
./tests/network/pair/run-test.sh
```

Run one subsystem group:

```bash
./tests/run-group.sh network
```

Run all root test groups:

```bash
./tests/run-all.sh
```

The complete run includes the 25-case model suite and the validation group.
Use a subsystem runner for a shorter development cycle.

The shared `tests/support/test-env.sh` file supplies repository, build, and
installation paths. Generated files keep their existing flat paths below
`build/tests/`.

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
