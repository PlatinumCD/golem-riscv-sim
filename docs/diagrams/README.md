# Architecture diagrams

These editable SVGs show implemented boundaries. Optional resources are labeled;
widths and capacities are configurable unless explicitly stated.

- [Single tile](single-tile-uml.svg): functional QEMU state, control/data bridges,
  SST timing, banked SPM, DMA, and analog arrays.
- [Mesh](mesh-topology.svg): a 3×3 example with X-then-Y routing from tile 0 to
  tile 8, local NIC lanes, and a separate global-RAM DMA fabric.

The drawings describe structure, not a deployment's resolved parameters.
See the [timing model](../timing-model.md),
[vector architecture](../vector-architecture.md), and
[source ownership](../../src/README.md) for details and implementation links.
