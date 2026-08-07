# Testing

Each test owns its sources and runs in the foreground. The repository uses
Sculptor's RA-tree compiler pipeline. The former task-graph and island tests
were removed because their compiler interface no longer exists.

## Compiler and runtime proof

```bash
./tests/torch-mlir/run-test.sh
./tests/pytorch-single-core/run-test.sh
./tests/sculptor-ra-tree-single-tile/run-test.sh
./tests/runtime-library/run-test.sh
```

`sculptor-ra-tree-single-tile` exports a deterministic PyTorch linear layer,
builds its RA tree, plans and places a tile, outlines its boot and dispatch
routines, and emits a RISC-V object with the generated tile ABI.

The shared RA-tree entry points are:

```text
build-scripts/lower-sculptor-ra-tree.sh
build-scripts/build-sculptor-core-objects.sh
```

See [Sculptor RA-tree migration](sculptor-ra-tree-migration.md) for the full
compiler-to-runtime-to-simulator contract.

## Platform and simulator proof

```bash
./components/elements/mittens/tests/run-test.sh
./visualizer/tests/run-test.sh
./tests/hello/run-test.sh
./tests/riscv-vector/run-test.sh
./tests/cpu-timing-validation/run-test.sh
./tests/analog-instructions/run-test.sh
./tests/analog-ops/run-test.sh
./tests/analog-timing-validation/run-test.sh
./tests/analog-mesh-2x2/run-test.sh
./tests/analog-route-2x2/run-test.sh
./tests/analog-mesh-2x2-dual-array/run-test.sh
./tests/mesh-pair/run-test.sh
./tests/mesh-3x3/run-test.sh
./tests/network-timing-validation/run-test.sh
./tests/transmit-fanout/run-test.sh
./tests/memory-timing-validation/run-test.sh
./tests/memory-hierarchy-l1/run-test.sh
./tests/deployment-runtime-pair/run-test.sh
./tests/mesh-pipeline/run-test.sh
./tests/distributed-matvec/run-test.sh
```

These tests validate the QEMU execution model, SST/Mittens mesh transport,
32-bit routed words, analog backends, timing synchronization, memory models,
and the reusable runtime independently of a particular neural-network model.
## Sculptor model-family simulation

`tests/sculptor-ra-tree-model-suite` adapts all 25 non-GPT Python model
fixtures from the pinned Sculptor repository. Each case lowers a PyTorch model,
creates a bare-metal tile ELF, runs it with QEMU and SST, and records SST
simulated completion time. See its `README.md` for commands and scope.
