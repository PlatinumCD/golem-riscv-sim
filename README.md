# Golem RISC-V simulation

Correctness and regression checks live in [`tests/`](docs/testing.md).
Architecture experiments and their dashboards live in [`studies/`](studies/README.md).
Use `python3 studies/run.py --list` to discover runnable studies.

This repository reproduces an AArch64-hosted compiler and simulation stack for
the bare-metal Golem RISC-V tile platform. It builds the custom LLVM/MLIR
compiler, a pinned PyTorch/Torch-MLIR import and lowering path, the
Sculptor-MLIR layer compiler, QEMU with the Mittens fd 41 execution
synchronizer, mesh NIC, and Golem analog instruction decoder, SST Core, and the
memHierarchy, Merlin, and Mittens SST elements. Each
simulated tile is an independent single-hart QEMU process with private memory.
Managed harts support RVV 1.0 with configurable `VLEN` and `ELEN`.
Mittens contains a complete functional analog accelerator path:
configurable tile-local arrays, selectable native C++ and CrossSim float32 MVM
backends, QEMU/SST shared-memory command transport, and modeled analog
transfers. The platform
contract gives every analog array an ordered four-command queue while all
arrays on a tile share one round-robin, bidirectional 256-bit link.

Sculptor uses its RA-tree deployment path. The repository cross-compiles the
pinned Sculptor tile runtime for RISC-V, while QEMU and SST remain the
functional and timing models for a tile and its mesh. See
[`docs/sculptor-ra-tree-migration.md`](docs/sculptor-ra-tree-migration.md) for
the compiler-to-simulator boundary and migration status.

SST is the simulation-time authority. Managed QEMU tiles execute precise
instruction-count grants through fd 41; Platform v0.1 initially models one
retired RISC-V instruction as one `cpu_clock` cycle. fd 42 carries mesh data
and fd 43 carries analog data.
Tiles optionally replace native QEMU data-memory timing with a private SST
memHierarchy L1 while keeping QEMU as the functional owner of RAM bytes.

Platform v0.1 is deliberately bare-metal. Linux, MUSL, pthreads, and OpenMP are
not part of the required build or runtime.

## Quick start

For hardware development with the existing pinned toolchain, SST Core, Merlin,
memHierarchy and CrossSim installations, run:

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
```

Bootstrap uses every online host CPU by default. Override that with `JOBS`:

```bash
JOBS=8 ./bootstrap.sh
```

The default bootstrap action builds and tests hardware. Missing dependencies
are reported rather than silently rebuilt; this workflow does not claim a
dependency-free fresh checkout. See [building](docs/building.md) for scope.
Compiler/model suites are explicit opt-ins. Useful hardware entry points are:

```bash
python3 -B tools/hardware/verify.py
bash tests/run-all.sh --list
bash tests/run-all.sh --case component
./tests/runtime/library/run-test.sh
./tests/memory/global-ram/run-test.sh
./tests/runtime/deployment-pair/run-test.sh
./tests/platform/hello/run-test.sh
./tests/platform/riscv-vector/run-test.sh
./tests/platform/cpu-timing/run-test.sh
./tests/analog/instructions/run-test.sh
./tests/analog/ops/run-test.sh
./tests/analog/timing/run-test.sh
./tests/analog/mesh-2x2/run-test.sh
./tests/analog/route-2x2/run-test.sh
./tests/analog/mesh-2x2-dual-array/run-test.sh
./tests/network/pair/run-test.sh
./tests/network/mesh-3x3/run-test.sh
./tests/network/timing/run-test.sh
./tests/network/transmit-fanout/run-test.sh
./tests/memory/scratchpad-dma/run-test.sh
./tests/network/pipeline/run-test.sh
./tests/analog/distributed-matvec/run-test.sh
```

The frozen component evidence gates can also be rerun together, without
building or executing the GPT-2 sweep:

```bash
./tests/validation/epoch-c/run-test.sh
```

Run one subsystem group with:

```bash
./tests/run-group.sh network
```

Run the hardware gate (default), or explicitly select the entire repository suite:

```bash
bash tests/run-all.sh --suite hardware
# Explicitly includes compiler and model workloads:
bash tests/run-all.sh --suite all
```

## Repository layout

```text
build-scripts/                  reproducible preparation and build operations
src/bridge/                     shared QEMU/SST bridge ABI
src/qemu/devices/               project-owned QEMU device sources
src/sst/                        project-owned SST element and component tests
src/qemu/instructions/          project-owned RISC-V decoder/helper sources
src/config/                     pinned revisions and Platform v0.1 build settings
docs/                           platform, build, and testing documentation
src/patches/qemu/               minimal upstream QEMU integration changes
src/platform/                   bare-metal startup, linker script, and MMIO API
tests/platform/                 boot, ISA, and CPU tests
tests/compiler/                 compiler integration tests
tests/runtime/                  tile runtime tests
tests/memory/                   global RAM and scratchpad/DMA tests
tests/network/                  mesh and transport tests
tests/analog/                   analog accelerator tests
tests/models/                   complete model deployments
tests/validation/               cross-component validation tests
tests/support/                  shared test infrastructure
third_party/                    pinned compilers, runtime source, and simulators
build/                          generated source and build trees (ignored)
install/                        generated local installation (ignored)
```

The upstream submodules remain pristine. Preparation scripts create detached
Git worktrees below `build/sources/`, overlay project-owned code, and apply the
small integration patches there.

## Documentation

- [`docs/architecture.md`](docs/architecture.md) explains component ownership,
  lifecycle, and the complete packet data path.
- [`docs/sculptor-ra-tree-migration.md`](docs/sculptor-ra-tree-migration.md)
  defines the current compiler, runtime, QEMU, and SST boundary.
- [`docs/platform-v0.1.md`](docs/platform-v0.1.md) defines the guest-visible address
  map, NIC registers, and mesh-routing contract.
- [`docs/platform-v0.2.md`](docs/platform-v0.2.md) defines the optional private
  scratchpad, its DMA interface, and its compiler and runtime ABI.
- [`docs/analog-isa.md`](docs/analog-isa.md) records the LLVM-defined Golem
  analog instruction encodings and the QEMU implementation contract.
- [`docs/vector-architecture.md`](docs/vector-architecture.md) locks the RVV
  1.0 ISA, 256-bit vector geometry, issue boundary, compiler contract, analog
  relationship, and timing ownership for a conforming Golem tile.
- [`docs/timing-model.md`](docs/timing-model.md) defines which timing and
  performance claims are valid in the current QEMU/SST integration.
- [`docs/building.md`](docs/building.md) documents host requirements and build
  entry points.
- [`docs/testing.md`](docs/testing.md) describes every system and element proof.
- [`docs/research-goals.md`](docs/research-goals.md) separates simulator-credibility work
  from the analog-versus-digital architectural study.
- [`docs/architecture-review.md`](docs/architecture-review.md) records the
  current experimental architecture, timing parameters, and claim limits.
- [`docs/compiler-scoring-advice.md`](docs/compiler-scoring-advice.md) gives
  evidence-backed guidance for Sculptor's placement and timing model.
- [`src/config/epoch-c.env`](src/config/epoch-c.env) preserves the first
  component-validated experimental baseline. Epoch C used a coarse
  4,096-word network timing cell.
- [`src/config/epoch-d.env`](src/config/epoch-d.env) is the corrected physical-word
  network baseline: one 32-bit timing cell with the same 64 KiB router
  capacity.
- [`src/config/epoch-e.env`](src/config/epoch-e.env) preserves the physical-word
  network model while making 100 ns the deployment and compiler cost of one
  analog MVM. Epoch C and Epoch D remain frozen historical baselines.

## Pinned upstreams

- PlatinumCD LLVM `golem-analog` at `f3e6ed545`;
- PlatinumCD Torch-MLIR `analog-extension` at `c36979b32`;
- PlatinumCD Sculptor-MLIR `master` at `cdfcc7a58`;
- PyTorch CPU `2.10.0+cpu`;
- Sandia CrossSim `v3.2.1`;
- QEMU `v8.2.2`;
- SST Core `v16.0.0_Final`; and
- SST Elements `v16.0.0_Final`.

Exact commit IDs are recorded in `src/config/versions.env` and by the Git
submodule links.

The runtime directory contains the implemented Platform v0.1 foundations:
tensor/task ABI types, immutable registries, the fixed task-instance pool,
the ready queue, and a transport-neutral 32-bit word interface. The complete
scheduler and tensor arena remain later integration stages; the distributed
matvec proof currently exercises the fixed word stream directly from its tile
application.
