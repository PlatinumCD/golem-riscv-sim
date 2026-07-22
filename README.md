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

The build succeeds only after both system proofs pass:

```bash
./tests/hello/run-test.sh
./tests/mesh-pair/run-test.sh
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
tests/                          complete system test scenarios
third_party/                    pinned, pristine Git submodules
build/                          generated source and build trees (ignored)
install/                        generated local installation (ignored)
```

The upstream submodules remain pristine. Preparation scripts create detached
Git worktrees below `build/sources/`, overlay project-owned code, and apply the
small integration patches there.

## Pinned upstreams

- PlatinumCD LLVM `golem-analog` at `d5685386e`;
- QEMU `v8.2.2`;
- SST Core `v16.0.0_Final`; and
- SST Elements `v16.0.0_Final`.

Exact commit IDs are recorded in `config/versions.env` and by the Git
submodule links.

See [`docs/platform-v0.md`](docs/platform-v0.md) for the locked guest-visible
address map and NIC interface.
