# Architecture diagrams

These SVGs are editable source diagrams of the tile and mesh boundaries. They
are illustrative, not a complete specification of every configurable resource.

- [Single tile](single-tile-uml.svg): guest, QEMU, bridge, SST, and analog boundaries.
- [Mesh](mesh-topology.svg): a 3×3 example with a route from tile 0 to tile 8.

The diagrams include Merlin-era network details. The current hardware path
also provides the Mittens wormhole NIC/router and configurable TX/RX lanes;
do not use these drawings to infer current lane counts or default buffering.

For current implementation ownership, see [src/README.md](../../src/README.md)
and [the SST element](../../src/sst/README.md).
