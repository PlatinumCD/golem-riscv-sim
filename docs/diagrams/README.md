# Architecture diagrams

These two editable SVGs are the canonical drawings used by the
[project overview](../../README.md). Update these files when the architecture
changes; do not maintain a second set of tile or mesh diagrams.

- [Single tile](single-tile.svg): current `src` CPU, instruction cache,
  shared CPU memory port, vector load/store queue, analog command queue,
  analog arrays, shared banked SPM, NIU, NIC and external Mordred router.
- [Mesh](mesh-connections.svg): a 3×3 example with X-then-Y routing from tile 0
  to tile 8. Each tile has its own router; intermediate CPUs do not forward data.

The tile drawing uses module and connection names without parameter values, formulas, widths,
capacities or queue depths. Repeated bank and array outlines indicate replication
without specifying counts. The mesh drawing remains an example topology.

Solid arrows describe functional data/request/response connections; dashed
arrows describe control. Router buffers and switching blocks aggregate the
router's ports. The NIC connects through the router's local port.
Blocking scalar loads/stores are drawn as a connection from the
scalar registers, while eligible vector accesses use the vector load/store queue.
Both paths and instruction-cache fills join one memory port on the CPU boundary.
The SPM bus represents the composition's routing bus. Bank/channel scheduling
is drawn inside the SPM controller to represent its nested banked backend;
controller ordering and backend scheduling supply the memory service model.

The topology follows these implementations and contracts:

- [Tile composition](../../src/components/mordred/tiles.py) and
  [local component wiring](../../src/configuration.py).
- [CPU and queues](../../src/components/riscv-qemu/README.md).
- [Analog commands and pipeline](../../src/components/analog-arrays/README.md).
- [SPM bank service](../../src/components/scratchpad/README.md).
- [NIU, receive storage and credits](../../src/components/mordred/spm-interface.md).
- [Mordred NIC and routers](../../src/components/mordred/configuration.py).

CPU and NIU requests enter the same SPM bus, controller and physical
banks. Bank access lists select which banks each client may use. The arrays'
payload connection ends at CPU vector registers. Scalar compute launch reaches
the array command queue separately from the CPU's queued vector transfers.

Current studies launch network requests through a finite external study
controller and consume destination arrival notifications there. The controller
and QEMU/SST host synchronization are simulation infrastructure and are described
in the README rather than drawn as tile hardware. The figure does not imply a
guest CPU send instruction or direct CPU access to another tile's SPM.
