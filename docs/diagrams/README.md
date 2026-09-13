# Architecture diagrams

These two editable SVGs are the canonical drawings used by the
[project overview](../../README.md). Update these files when the architecture
changes; do not maintain a second set of tile or mesh diagrams.

- [Single tile](single-tile.svg): instruction cache, executable SPM, DMA queues,
  external router, and the separate shared-memory path.
- [Mesh](mesh-connections.svg): a 3×3 example with X-then-Y routing from tile 0
  to tile 8. Each tile has its own router; intermediate CPUs do not forward data.

Capacities shown on the tile are defaults, not fixed hardware limits. The
3×3 mesh is an example. The drawings show architectural data paths; QEMU/SST
host transports are explained in the [bridge guide](../../src/bridge/README.md).
