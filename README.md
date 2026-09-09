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
See the [parameter reference](docs/parameters.md).

## First run

Use an AArch64 Linux host with the build tools checked by `bootstrap.sh check`.
From a fresh checkout:

```bash
git clone https://github.com/PlatinumCD/golem-riscv-sim.git
cd golem-riscv-sim
bash bootstrap.sh check
JOBS=8 bash bootstrap.sh build
bash tests/run-all.sh --case platform/hello
```

`build` acquires and builds the shared dependencies, then builds the current
hardware. The hello test boots one bare-metal tile and prints
`Golem: single tile booted`. See [tests/](tests/README.md) for the coverage map.

After changing simulator code:

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
```

`build-hardware` reuses installed dependencies. The build prints its source,
build, and installation paths; tests check that the loaded simulator matches
the current source. `bash bootstrap.sh --help` lists individual build actions.

```bash
bash tests/run-all.sh --list
bash tests/run-all.sh --case component
bash tests/run-group.sh network
python3 -B tools/hardware/verify.py
```

## Source layout

| Directory | Contents |
|---|---|
| [src/](src/README.md) | QEMU devices, SST models, bridge protocols, and guest support |
| [tools/hardware/](tools/hardware/README.md) | Build checks, test runner, and baseline comparisons |
| [tools/analysis/](tools/analysis/README.md) | Performance, progress, and result analysis |
| [tools/compiler/](tools/compiler/README.md) | Optional compiler artifact checks |
| [tests/](tests/README.md) | Integration and regression tests |
| build-scripts/ | Dependency preparation and build scripts |
| docs/ | Architecture and interface documentation |
| third_party/ | Upstream submodules |

Shared dependencies use `build/<dependency>/` and `install/<dependency>/`.
The current hardware uses `build/src/` and `install/src/`.
Test output goes under `tests/results/`; all these outputs are ignored.

For optional compiler integration, provide the Sculptor source directory with
`GOLEM_SCULPTOR_SOURCE=/absolute/path` when running `bash bootstrap.sh compiler`.

## Documentation

- [Architecture](docs/architecture.md): component ownership and data paths.
- [Hardware tooling](tools/hardware/README.md): build checks and test execution.
- [Clock and counter semantics](src/sst/execution/TIMING.md): interpreting measurements.
- [Platform](docs/platform.md): guest-visible devices, scratchpad, DMA, and communication.
- [RVV](docs/vector-architecture.md) and [analog instructions](docs/analog-isa.md).

Simulated time is not host runtime. Service and stall counters can overlap;
do not add them together and call the sum elapsed time.
