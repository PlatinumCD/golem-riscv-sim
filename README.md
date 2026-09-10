# RISC-V Tile Simulator

This project simulates a grid of RISC-V tiles. Each tile runs a program,
performs scalar and vector computation, and keeps local data in a scratchpad.
Tiles exchange data through a mesh network. Optional analog arrays provide
matrix computation.

## Architecture

### Inside one tile

![Single tile architecture](docs/diagrams/single-tile.svg)

A tile is one compute unit with its own processor and local memory. It contains:

- **A RISC-V processor** that runs the tile's program. Scalar instructions work
  on individual values; vector instructions work on several values at once.
- **A scratchpad** that holds the tile's local data. The program decides what
  goes there. The memory is divided into banks, allowing independent accesses
  when their read and write ports are available.
- **TX and RX DMA engines** that send and receive scratchpad data while the
  processor can continue computing. Each TX lane has its own send queue; RX takes
  arriving data from receive queues. These queues have limited space: when full,
  they make incoming data wait. Both engines share the scratchpad with the processor.
- **A network interface** that connects those engines to the tile's mesh router,
  shown outside the tile boundary.
- **Global-memory DMA** that transfers data between the scratchpad and shared
  main memory through a separate memory controller, not through the mesh.
- **Optional analog arrays** for matrix computation.

You can vary the processor's vector width and scalar issue rate, the scratchpad's
capacity, banking, ports and access timing, the number of simultaneous sends
and receives, and the analog arrays' dimensions and timing.

### Connecting tiles

Tiles form a two-dimensional mesh. Messages pass through neighboring routers
to reach another tile's scratchpad. You can vary the grid dimensions, link
bandwidth, router delay, and queue capacity. Transfers that need the same busy
link must wait.

Tiles can also transfer data to and from shared global memory through a memory
controller.

### Running the simulation

QEMU runs each tile's program; SST calculates simulated execution and transfer
time. Processor timing is based on instruction accounting, not a detailed
processor pipeline.

## Build and run

This project has only been tested on ARM processors so far.
The first command checks the required build tools:

```bash
bash bootstrap.sh check
JOBS=8 bash bootstrap.sh build
bash tests/run-all.sh --case platform/hello
```

Run these from the repository root. The build prepares the dependencies and
simulator. The test then boots one tile and prints `Golem: single tile booted`.

## Learn more

- [Understand the machine](docs/architecture.md): components and how data moves between them.
- [Configure the machine](docs/parameters.md): parameter names, units, and defaults.
- [Run the tests](tests/README.md): what is checked and how to run it.
