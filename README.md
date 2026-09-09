# Golem RISC-V Simulator

Golem simulates a mesh of RISC-V tiles with vector execution, private banked
scratchpads, DMA communication, and optional analog matrix-compute arrays.

QEMU executes each tile's bare-metal program. SST models instruction time,
memory service, network traffic, and accelerator timing. The CPU is an
instruction-accounting model, not a detailed processor pipeline.

## Hardware model

- **Compute:** scalar RISC-V and RVV, with configurable vector length and scalar issue width.
- **Local memory:** banked, noncoherent scratchpad with separate read/write ports.
- **Communication:** 1, 2, or 4 TX and RX streams sharing the existing scratchpad and network resources.
- **Network:** a configurable 2-D mesh with XY routing, finite buffers, and backpressure.
- **Global memory:** explicit DMA transfers through a shared memory controller.
- **Analog:** configurable arrays with native or CrossSim numerical backends.

Hardware settings belong to the simulation configuration. Study-specific
capacities, clock rates, and bandwidths are not fixed properties of the machine.
See [parameter definitions](src/sst/configuration/tileParameters.h).

## Build and test

With the toolchain and shared dependencies already installed:

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
```

This rebuilds the project-owned hardware integration. It does not install the
compiler toolchain, SST Core, or other shared dependencies. For a new machine,
start with [the build guide](docs/building.md).

```bash
bash tests/run-all.sh --list
bash tests/run-all.sh --case component
bash tests/run-group.sh network
python3 -B tools/hardware/verify.py
```

The hardware suite currently includes a communication-envelope wrapper that
requires the local-only `studies/` tree. A checkout without that tree cannot
run the complete suite as-is; see [the wrapper's README](tests/network/communication-envelope/README.md).

## Source layout

| Directory | Contents |
|---|---|
| [src/](src/README.md) | QEMU devices, SST models, bridge protocols, and guest support |
| [tools/hardware/](tools/hardware/README.md) | Build checks, test runner, and baseline comparisons |
| tests/ | Integration and regression tests |
| build-scripts/ | Dependency preparation and build scripts |
| docs/ | Architecture and interface documentation |
| third_party/ | Upstream submodules |

Builds and installations go under `build/` and `install/`; hardware defaults
are `build/src/` and `install/src/`. Test output normally goes under
`tests/results/`. These generated directories are ignored.

`studies/`, `tests/compiler/`, and `tests/models/sculptor-ra-tree/` are
local-only directories excluded from Git tracking. Compiler-dependent tests
elsewhere may still require them. Submodule edits also require separate commits.

## Documentation

- [Architecture](docs/architecture.md): component ownership and data paths.
- [Building](docs/building.md): prerequisites and build actions.
- [Testing](docs/testing.md): test selection and execution.
- [Clock and counter semantics](src/sst/execution/TIMING.md): interpreting measurements.
- [Guest interfaces](docs/platform-v0.1.md) and [scratchpad DMA](docs/platform-v0.2.md).
- [RVV](docs/vector-architecture.md) and [analog instructions](docs/analog-isa.md).

Simulated time is not host runtime. Service and stall counters can overlap;
do not add them together and call the sum elapsed time.
