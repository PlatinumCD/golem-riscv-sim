# Golem RISC-V simulation

This repository reproduces an AArch64-hosted compiler and simulation stack for
the bare-metal Golem RISC-V tile platform. It builds the custom LLVM/MLIR
compiler, QEMU with the Mittens mesh NIC, SST Core, and the Merlin and Mittens
SST elements. Each simulated tile is an independent single-hart QEMU process
with private memory.

Platform v0 is deliberately bare-metal. Linux, MUSL, pthreads, and OpenMP are
not part of the required build or runtime.

## Quick start

After installing the host dependencies documented in
[`docs/building.md`](docs/building.md), run:

```bash
git clone --recurse-submodules https://github.com/PlatinumCD/golem-riscv-sim.git
cd golem-riscv-sim
./bootstrap.sh
```

Bootstrap uses every online host CPU by default. Override that with `JOBS`:

```bash
JOBS=8 ./bootstrap.sh
```

The build succeeds only after the runtime unit test and all six target/system
proofs pass:

```bash
./runtime/tests/run-test.sh
./tests/runtime-library/run-test.sh
./tests/hello/run-test.sh
./tests/mesh-pair/run-test.sh
./tests/mesh-3x3/run-test.sh
./tests/mesh-pipeline/run-test.sh
./tests/distributed-matvec/run-test.sh
```

## Repository layout

```text
build-scripts/                  reproducible preparation and build operations
bridge/                         shared QEMU/SST bridge ABI
components/devices/             project-owned QEMU device sources
components/elements/mittens/    project-owned SST element and its tests
config/                         pinned revisions and Platform v0 build settings
docs/                           platform, build, and testing documentation
patches/qemu/                   minimal upstream QEMU integration changes
platform/                       bare-metal startup, linker script, and MMIO API
runtime/                        freestanding task/dataflow runtime library
tests/                          complete system test scenarios
third_party/                    pinned, pristine Git submodules
build/                          generated source and build trees (ignored)
install/                        generated local installation (ignored)
```

The upstream submodules remain pristine. Preparation scripts create detached
Git worktrees below `build/sources/`, overlay project-owned code, and apply the
small integration patches there.

## Documentation

- [`docs/architecture.md`](docs/architecture.md) explains component ownership,
  lifecycle, and the complete packet data path.
- [`docs/platform-v0.md`](docs/platform-v0.md) defines the guest-visible address
  map, NIC registers, and mesh-routing contract.
- [`docs/timing-model.md`](docs/timing-model.md) defines which timing and
  performance claims are valid in the current QEMU/SST integration.
- [`docs/building.md`](docs/building.md) documents host requirements and build
  entry points.
- [`docs/testing.md`](docs/testing.md) describes every system and element proof.

## Pinned upstreams

- PlatinumCD LLVM `golem-analog` at `d5685386e`;
- QEMU `v8.2.2`;
- SST Core `v16.0.0_Final`; and
- SST Elements `v16.0.0_Final`.

Exact commit IDs are recorded in `config/versions.env` and by the Git
submodule links.

The runtime directory contains the implemented Platform v0 foundations:
tensor/task ABI types, immutable registries, the fixed task-instance pool,
the ready queue, and a transport-neutral 32-bit word interface. The complete
scheduler and tensor arena remain later integration stages; the distributed
matvec proof currently exercises the fixed word stream directly from its tile
application.
