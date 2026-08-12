# Golem RISC-V Sim — Quick Reference

## One-liner
A bare-metal RISC-V simulation platform with an analog accelerator (Mittens), connecting QEMU compute tiles via an SST event-driven fabric.

## What it does
- Simulates tiled RISC-V cores with RVV 1.0 + analog MVM ops
- Each tile = 1 QEMU hart + private memory, communicating via MMIO
- Compiler chain: PyTorch → Torch-MLIR → Sculptor task graph → LLVM/Golem ISA
- Simulation runtime: SST for network/memory timing; fd 41/42/43 for control/data/analog

## Build
```bash
./bootstrap.sh          # default: all CPUs
JOBS=8 ./bootstrap.sh   # with job limit
```

## Key dirs
| Dir | Purpose |
|---|---|
| `components/` | QEMU devices, SST element (mittens), RISC-V decoder |
| `bridge/` | QEMU↔SST shared-memory ABI |
| `platform/` | Bare-metal startup, linker script, MMIO API |
| `runtime/` | Task/dataflow runtime lib |
| `tests/` | System test scenarios |
| `visualizer/` | Mesh activity animation + timeline view |
| `third_party/` | Pinned upstream submodules (LLVM, Torch-MLIR, etc.) |
| `config/` | Pinned revisions, build env files |

## Core subsystems
1. **Compute**: QEMU (patched) + LLVM/Golem custom ISA
2. **Analog accelerator**: Mittens SST element — C++ or CrossSim MVM backends
3. **Network**: 256-bit bidirectional link, round-robin scheduling
4. **Simulation**: SST event-driven engine with memHierarchy/L1 optionality

## Upstream pins
- LLVM: `f3e6ed545`
- Torch-MLIR: `c36979b32`
- Sculptor-MLIR: `cdfcc7a58`
- PyTorch CPU: `2.10.0+cpu`
- QEMU: `v8.2.2`
- SST: `v16.0.0_Final`

## Docs of note
- `docs/architecture.md`
- `docs/analog-isa.md`
- `docs/compiler-workflow.md`
- `docs/timing-model.md`