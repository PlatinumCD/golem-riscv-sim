# Architecture diagrams

These SVGs document an earlier Platform v0.1 integration. They are editable
source diagrams, not an up-to-date specification of every configurable resource.

- [Single tile](single-tile-uml.svg): guest, QEMU, bridge, SST, and analog boundaries.
- [Mesh](mesh-topology.svg): a 3×3 example with a route from tile 0 to tile 8.

The diagrams include Merlin-era network details. The current hardware path
also provides the Mittens wormhole NIC/router and configurable TX/RX lanes;
do not use these drawings to infer current lane counts or default buffering.

For current implementation ownership, see [src/README.md](../../src/README.md)
and [the SST element](../../src/sst/README.md).
