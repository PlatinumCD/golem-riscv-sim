# QEMU integration

QEMU executes instructions and maintains their functional bytes and registers.
SST decides when instruction fetches, data accesses and device operations
complete. QEMU's host backing is not an extra architectural memory.

| Source | Role |
|---|---|
| [devices/mittens-sync/](devices/mittens-sync/) | Instruction grants, fetch/data records, yield/resume and scratchpad/global-DMA device state |
| [devices/mittens-nic/](devices/mittens-nic/README.md) | Guest network registers, payload snapshots and authorized receive copies |
| [devices/mittens-analog/](devices/mittens-analog/README.md) | Analog command and result transport |
| [instructions/golem-analog/](instructions/golem-analog/README.md) | Decode and implement custom instruction operands |

The [SST launcher](../sst/execution/qemuProcess.cc) starts one managed QEMU per
active tile. Scratchpad boot enables the executable SPM mapping and dynamic
instruction-fetch reporting. The
[instruction cache](../sst/memory/instructionCache.cc) and
[SPM arbiter](../sst/memory/scratchpad/scratchpadTimingModel.cc) live in SST.

[prepare-qemu.sh](../../build-scripts/prepare-qemu.sh) copies these sources and
applies [integration patches](../patches/README.md) to the pinned dependency.
Change this directory or those patches, not the generated QEMU build tree.
Shared record definitions belong in [bridge/](../bridge/README.md); update
both QEMU and SST when changing a record.
