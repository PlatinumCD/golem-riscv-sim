# Golem RISC-V Simulation — Project Summary

## Overview
A simulation and compiler stack for a **bare-metal Golem RISC-V tile platform** running on AArch64 hosts. It integrates QEMU (customized with analog instruction support), SST (SystemSimulator) for event-driven simulation, and a multi-layer compiler toolchain.

## Key Components

### Hardware Architecture
- **Tiles**: Each simulated tile is an independent single-hart QEMU process with private memory
- **ISA**: RISC-V with RVV 1.0 support (configurable VLEN/ELEN)
- **Analog Accelerator ("Mittens")**: Configurable tile-local arrays with selectable C++ / CrossSim MVM backends
- **Network**: Round-robin, bidirectional 256-bit link shared by all arrays on a tile
- **MMIO**: fd 41 = execution grants, fd 42 = mesh data, fd 43 = analog data

### Software Stack
| Layer | Tool |
|---|---|
| MLIR compiler | Custom LLVM/MLIR (`golem-analog` branch) |
| PyTorch import | Torch-MLIR (`analog-extension` branch) |
| Task graph | Sculptor-MLIR |
| Execution | QEMU v8.2.2 (patched) |
| Simulation engine | SST Core v16.0.0_Final + SST Elements |

### Timing Model
- Platform v0.1: 1 retired RISC-V instruction = 1 `cpu_clock` cycle (initial)
- Memory: Optional private SST memHierarchy L1; QEMU owns RAM bytes
- No OS — bare-metal only (no Linux, MUSL, pthreads, or OpenMP)

### Repository Structure
```
build-scripts/          # Reproducible build ops
bridge/                # Shared QEMU/SST bridge ABI
components/
  devices/           # Project-owned QEMU device sources
  elements/mittens/  # SST element + tests
  qemu/              # RISC-V decoder/helper sources
config/               # Pinned revisions, build settings
docs/                 # Platform spec, timing, architecture
patches/qemu/        # Minimal upstream QEMU integration changes
platform/             # Bare-metal startup, linker script, MMIO API
runtime/              # Task/dataflow runtime library
tests/                # System test scenarios
third_party/         # Pinned upstream submodules
visualizer/          # Mesh activity exporter + web viewer
build/               # Generated source/build trees
install/             # Generated local installation
```

## Phases / Baselines
- **Epoch C**: Coarse 4,096-word network timing cell
- **Epoch D**: Corrected physical-word network model
- **Epoch E**: Physical-word network + 100 ns cost per analog MVM

## Upstream Submodules (pristine)
- PlatinumCD LLVM `golem-analog` at `f3e6ed545`
- PlatinumCD Torch-MLIR `analog-extension` at `c36979b32`
- PlatinumCD Sculptor-MLIR `master` at `cdfcc7a58`
- PyTorch CPU `2.10.0+cpu`
- Sandia CrossSim `v3.2.1`
- QEMU `v8.2.2`
- SST Core `v16.0.0_Final`
- SST Elements `v16.0.0_Final`

## Testing
Run all validation tests:
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

## Docs
- `docs/architecture.md` — component ownership, lifecycle, packet data path
- `docs/platform-v0.1.md` — guest-visible address map, NIC registers, mesh-routing
- `docs/analog-isa.md` — Golem analog instruction encodings
- `docs/vector-architecture.md` — RVV 1.0 ISA, 256-bit vector geometry
- `docs/compiler-workflow.md` — PyTorch → Sculptor → Golem lowering pipeline
- `docs/timing-model.md` — valid timing/performance claims
- `docs/research-goals.md` — simulator credibility vs. architectural study