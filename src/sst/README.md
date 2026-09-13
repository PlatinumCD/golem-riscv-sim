# Mittens SST element

Mittens connects QEMU-backed RISC-V tiles to SST memory, network, and analog
models. `mittens.tile` owns each tile's lifecycle and delegates resource work
to the controllers below.

| Directory | Responsibility |
|---|---|
| configuration/ | Read, validate, and export machine settings |
| execution/ | QEMU grants, instruction accounting, replay, and CPU wakeups |
| bridge/ | Host shared-memory transport |
| memory/ | Scratchpad arbitration, instruction cache, boot images, and global RAM |
| network/ | TX/RX controllers, DMA, NIC, and wormhole router |
| analog/ | Command queues, shared analog link, and numerical backends |
| profiling/ | Counters, task traces, and measurement output |
| synchronization/ | Sideband epoch coordination |
| tile/ | SST component lifecycle and controller wiring |
| probes/ | Synthetic requesters used by component tests |
| tests/ | Component correctness tests |

## Execution

With `launch_mode=managed`, a tile launches one QEMU process. SST grants
instruction budgets and processes the reported memory and device boundaries
at their modeled times. Executable-SPM execution uses single issue;
vector issue remains limited to one instruction per cycle.

Execution is conservative and single-issue; host parallelism does not multiply
the modeled CPU bandwidth.
See [execution control and clocks](execution/README.md).

## Memory and network

All managed programs execute from scratchpad.
Code, data and stack occupy SPM; instructions pass through an 8 KiB cache.
Cache fills, CPU data accesses and DMA use the common SPM arbiter;
the CPU waits for the modeled boot DMA before starting. See
[boot and cache configuration](../../docs/architecture.md#scratchpad-backed-instruction-cache).

The wormhole router uses XY routing and finite credited buffers. TX and RX
each support 1, 2, or 4 local lanes. These lanes share the four physical
directional links. Packet ownership lasts through the tail, so requests for
the same output can block one another.

TX reads scratchpad data through modeled service into a bounded FIFO. RX
writes through the same scratchpad arbiter. Functional bridge copies are not
substitutes for those timing operations. See [RX ownership](network/rx/README.md).

## Analog

A nonzero array count enables tile-local matrix-compute arrays. Arrays have
separate command queues and compute state but share a bidirectional 256-bit
transfer link. Native and CrossSim backends supply numerical results.

## Configuration and checks

[Tile parameters](configuration/tileParameters.h) are the source of truth for
names and defaults; [configuration](configuration/README.md) explains snapshots.

From the repository root:

```bash
python3 -B tools/hardware/verify.py
bash tests/run-all.sh --case configuration
bash tests/run-all.sh --case component
```

The first command runs host checks. The latter commands require the installed
simulation dependencies. Measurement availability and aggregation rules are
defined in [the measurement contract](profiling/MEASUREMENT_CONTRACT.md).
