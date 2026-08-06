# Golem RISC-V simulation

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

SST is the simulation-time authority. Managed QEMU tiles execute precise
instruction-count grants through fd 41; Platform v0.1 initially models one
retired RISC-V instruction as one `cpu_clock` cycle. fd 42 carries mesh data
and fd 43 carries analog data.
Tiles optionally replace native QEMU data-memory timing with a private SST
memHierarchy L1 while keeping QEMU as the functional owner of RAM bytes.

Platform v0.1 is deliberately bare-metal. Linux, MUSL, pthreads, and OpenMP are
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

The build succeeds only after the element, runtime, and target/system proofs
pass:

```bash
./components/elements/mittens/tests/run-test.sh
./visualizer/tests/run-test.sh
./tests/torch-mlir/run-test.sh
./tests/pytorch-single-core/run-test.sh
./tests/torch-mlir-sculptor/run-test.sh
./tests/sculptor-core-elf/run-test.sh
./tests/sculptor-four-layer-mesh/run-test.sh
./tests/sculptor-eight-layer-mesh/run-test.sh
./tests/sculptor-eight-layer-mesh-4x2/run-test.sh
./runtime/tests/run-test.sh
./tests/runtime-library/run-test.sh
./tests/memory-hierarchy-l1/run-test.sh
./tests/deployment-runtime-pair/run-test.sh
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
./tests/mesh-pipeline/run-test.sh
./tests/distributed-matvec/run-test.sh
```

The frozen component evidence gates can also be rerun together, without
building or executing the GPT-2 sweep:

```bash
./tests/epoch-c-validation/run-test.sh
```

## Repository layout

```text
build-scripts/                  reproducible preparation and build operations
bridge/                         shared QEMU/SST bridge ABI
components/devices/             project-owned QEMU device sources
components/elements/mittens/    project-owned SST element and its tests
components/qemu/                project-owned RISC-V decoder/helper sources
config/                         pinned revisions and Platform v0.1 build settings
docs/                           platform, build, and testing documentation
patches/qemu/                   minimal upstream QEMU integration changes
platform/                       bare-metal startup, linker script, and MMIO API
runtime/                        freestanding task/dataflow runtime library
tests/                          complete system test scenarios
third_party/                    pinned, pristine Git submodules
visualizer/                     opt-in mesh activity exporter and web viewer
build/                          generated source and build trees (ignored)
install/                        generated local installation (ignored)
```

The upstream submodules remain pristine. Preparation scripts create detached
Git worktrees below `build/sources/`, overlay project-owned code, and apply the
small integration patches there.

## Documentation

- [`docs/architecture.md`](docs/architecture.md) explains component ownership,
  lifecycle, and the complete packet data path.
- [`docs/platform-v0.1.md`](docs/platform-v0.1.md) defines the guest-visible address
  map, NIC registers, and mesh-routing contract.
- [`docs/platform-v0.2.md`](docs/platform-v0.2.md) defines the optional private
  scratchpad, its DMA interface, and its compiler and runtime ABI.
- [`docs/analog-isa.md`](docs/analog-isa.md) records the LLVM-defined Golem
  analog instruction encodings and the QEMU implementation contract.
- [`docs/vector-architecture.md`](docs/vector-architecture.md) locks the RVV
  1.0 ISA, 256-bit vector geometry, issue boundary, compiler contract, analog
  relationship, and timing ownership for a conforming Golem tile.
- [`docs/compiler-workflow.md`](docs/compiler-workflow.md) walks a concrete
  two-layer PyTorch model through Torch-MLIR, Sculptor task-graph construction,
  two-core scheduling, same-core task fusion, Golem shim/task-graph ABI
  lowering, runtime resource finalization, and LLVM-dialect task code.
- [`docs/timing-model.md`](docs/timing-model.md) defines which timing and
  performance claims are valid in the current QEMU/SST integration.
- [`docs/building.md`](docs/building.md) documents host requirements and build
  entry points.
- [`docs/testing.md`](docs/testing.md) describes every system and element proof.
- [`visualizer/README.md`](visualizer/README.md) documents the opt-in animated
  mesh and timeline view for tasks, tensor routes, DMA, analog work, and waits.
- [`docs/research-goals.md`](docs/research-goals.md) separates simulator-credibility work
  from the analog-versus-digital architectural study.
- [`docs/architecture-review.md`](docs/architecture-review.md) records the
  current experimental architecture, timing parameters, and claim limits.
- [`docs/compiler-scoring-advice.md`](docs/compiler-scoring-advice.md) gives
  evidence-backed guidance for Sculptor's placement and timing model.
- [`config/epoch-c.env`](config/epoch-c.env) and
  [`results/epoch-c-freeze-2026-07-30.md`](results/epoch-c-freeze-2026-07-30.md)
  preserve the first component-validated experimental baseline. Epoch C used
  a coarse 4,096-word network timing cell.
- [`config/epoch-d.env`](config/epoch-d.env) is the corrected physical-word
  network baseline: one 32-bit timing cell with the same 64 KiB router
  capacity.
- [`config/epoch-e.env`](config/epoch-e.env) preserves the physical-word
  network model while making 100 ns the deployment and compiler cost of one
  analog MVM. Epoch C and Epoch D remain frozen historical baselines.

## Pinned upstreams

- PlatinumCD LLVM `golem-analog` at `d5685386e`;
- PlatinumCD Torch-MLIR `analog-extension` at `c36979b32`;
- PlatinumCD Sculptor-MLIR `master` at `cdfcc7a58`;
- PyTorch CPU `2.10.0+cpu`;
- Sandia CrossSim `v3.2.1`;
- QEMU `v8.2.2`;
- SST Core `v16.0.0_Final`; and
- SST Elements `v16.0.0_Final`.

Exact commit IDs are recorded in `config/versions.env` and by the Git
submodule links.

The runtime directory contains the implemented Platform v0.1 foundations:
tensor/task ABI types, immutable registries, the fixed task-instance pool,
the ready queue, and a transport-neutral 32-bit word interface. The complete
scheduler and tensor arena remain later integration stages; the distributed
matvec proof currently exercises the fixed word stream directly from its tile
application.
