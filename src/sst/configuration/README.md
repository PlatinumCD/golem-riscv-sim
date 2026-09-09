# Resolved machine configuration

Configuration is read before resource execution and then treated as immutable.
The adapters preserve the existing SST names, defaults, and supported modes:

- `TileConfiguration`: CPU/RVV, SPM, TX/RX DMA, analog, guest launch and tracing.
  `tileParameters.h` supplies typed members, defaults, SST documentation and JSON
  output from the same 73 definitions. QEMU launch and SPM/analog construction
  consume those resolved fields; guest ABI addresses remain shared constants.
- `RouterConfiguration` and `NetworkInterfaceConfiguration`: dimensions,
  directional link width, TX lanes, buffering, pipeline and clock settings.
- `GlobalRAMConfiguration`: capacity, channels, arbitration/queue settings,
  dependency mode and active tiles. Sparse backing and execution queues remain
  owned by the memory controller.

`GOLEM_RESOLVED_CONFIG_DIR` enables per-instance JSON snapshots for these
resources. `src2/env.sh` and the comparison runner set it automatically. Names
include process and sequence IDs so several simulations cannot overwrite each
other. Snapshotting is host work and adds no simulated cycles.

Fields distinguish `hardware`, host `execution`, `measurement` and `workload`
settings. TX lanes share the same SPM banks and physical links. RX queue depth
is not RX-lane count: `rx_dma_streams=1|2|4` separately controls receive lanes,
including matching router/NIC local ejection paths. The default is still R1.
See `../network/rx/README.md` for resources and validation. The existing mesh
builder still wires the resources; this directory does not invent a new mapping
or replace workload placement. Defaults have not been widened or sped up.

Run `python3 src2/sst/tests/run-configuration-test.py` for rejection/default
checks, and `python3 src2/regression.py` for source/reference hardware checks.
An invalid test must match its intended diagnostic, not merely exit nonzero.
