# QEMU network device

`mittens-nic` is the guest MMIO interface to SST's network. It is attached to
the RISC-V virt machine at `0x10010000`. It handles guest data and bridge
state; routing and network timing belong to SST.

## Transfers

- Scalar TX writes enqueue one word with the selected destination.
- Burst TX snapshots a guest-memory range into the host bridge and reports
  its submission boundary through fd 41. SST applies the configured DMA and
  network timing; the host snapshot does not make transmission instantaneous.
- `TX_WAIT` blocks when the requested transmit ring is full.
- `RX_SOURCE` identifies the next payload; `RX_DATA` consumes it.
- `RX_WAIT` rechecks availability before yielding, avoiding a status/wait race.
- Receive DMA registers a source, route, destination address, and word count.
  Guest copying proceeds only after SST authorizes the transfer.

Task markers, initialization completion, and epoch arrival use control events,
not NoC payload traffic.

## Interface definitions

Register offsets and validation live in [mittens_nic.c](mittens_nic.c).
Use [mesh-nic.h](../../../platform/devices/mesh-nic.h) from guest code.
The shared host format is [NICTileBridge.h](../../../bridge/include/mittens/NICTileBridge.h).

These interfaces are different: guest MMIO issues operations; the host bridge
transports their data. Neither defines the physical router's buffering.

## Managed execution

SST supplies the per-tile network bridge as descriptor 42:

```text
-global mittens-nic.bridge-fd=42
```

The device validates and maps the bridge during realization. Execution yields
and resumes use the separate fd 41 bridge. Without a connected network bridge,
the device cannot send or receive.

Invalid accesses and transfer states report guest errors. A connected bridge
records the first protocol error for SST to diagnose.

The source is installed into the prepared QEMU worktree by
[prepare-qemu.sh](../../../../build-scripts/prepare-qemu.sh).
See [bridge ownership](../../../bridge/README.md).
