# QEMU/SST bridges

Each managed tile has its own shared-memory bridges. They connect host
processes; they are not physical links in the simulated machine.

| Child descriptor | Header | Role |
|---|---|---|
| 41 | [SyncTileBridge.h](include/mittens/SyncTileBridge.h) | Instruction grants, stop events, and resume |
| 42 | [NICTileBridge.h](include/mittens/NICTileBridge.h) | Network payloads and receive authorization |
| 43 | [AnalogTileBridge.h](include/mittens/AnalogTileBridge.h) | Analog commands, operands, and results |
| 44 | [Global RAM backing](../sst/memory/globalRAMBacking.h) | Shared-memory file backing; not a control protocol |

Only fd 41 controls guest execution. Publishing data on fd 42 or 43 does not
resume a hart.

## Ownership

SST creates the mappings and passes descriptors to QEMU. Both sides validate
the ABI before use. The C-compatible headers define layouts, versions, queue
capacities, and atomic operations; do not maintain a separate offset table here.

Producers write slot contents before publishing them with release ordering.
Consumers acquire the published state before reading. A slot cannot be reused
until its consumer releases it.

- **Synchronization:** SST grants an instruction budget; QEMU reports its
  executed counts and stop reason. SST schedules the event before resuming.
- **Network:** QEMU publishes transmit data; SST routes and times it.
  Receive DMA requires SST authorization before QEMU exposes the completed
  transfer to guest software.
- **Analog:** QEMU submits commands and operands. SST handles queue acceptance,
  transfer timing, computation, and completion. Per-array queues share one
  modeled tile-wide analog link.

Memory-event records describe instruction fetches and data accesses. QEMU
retains functional bytes while SST models service and contention. With
scratchpad boot, code/data/stack addresses refer to SPM; host backing is not
additional storage the program can use.

## Changing the ABI

Update the shared header and both consumers together. Layout changes need a
version change and matching validation tests. Never infer physical bandwidth
from a host ring's capacity or a bulk host copy.

Bridge tests are in [src/sst/tests](../sst/tests/). The guest MMIO interface is
separate; see [the NIC device](../qemu/devices/mittens-nic/README.md) and
[guest helpers](../platform/devices/mesh-nic.h).
