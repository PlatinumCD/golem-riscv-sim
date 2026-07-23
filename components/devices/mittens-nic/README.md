# Mittens QEMU NIC

The Mittens NIC is the guest-visible boundary between a bare-metal RISC-V
program running in QEMU and the SST mesh. It is a QEMU `SysBusDevice` with
type name `mittens-nic`, attached to the RISC-V `virt` machine at
`0x10010000`. The device exposes a 4 KiB MMIO window and moves packets through
the shared host bridge documented in
[`../../../bridge/README.md`](../../../bridge/README.md).

The NIC is deliberately small. It does not compute routes, know tile
coordinates, identify the sender to guest software, or model network timing.
Those responsibilities belong to the Mittens SST element and Merlin network.
See the complete data path in
[`../../../docs/architecture.md`](../../../docs/architecture.md).

## Guest-visible registers

All defined accesses are aligned, little-endian, 32-bit operations. The
register interface is also part of the Platform v0 contract in
[`../../../docs/platform-v0.md`](../../../docs/platform-v0.md).

| Offset | Name | Access | Meaning |
| ---: | --- | --- | --- |
| `0x00` | `STATUS` | Read | Bit 0 is `TX_READY`; bit 1 is `RX_VALID` |
| `0x04` | `TX_DEST` | Write | Latches the destination endpoint ID |
| `0x08` | `TX_DATA` | Write | Enqueues this payload with the latched destination |
| `0x0c` | `RX_DATA` | Read | Removes and returns the next received payload |
| `0x10`-`0xfff` | Reserved | — | Reads return zero; writes are ignored |

To transmit, software waits for `STATUS.TX_READY`, writes `TX_DEST`, executes
an I/O memory fence, and writes the 32-bit payload to `TX_DATA`. The
`TX_DATA` write is the doorbell: each write creates exactly one bridge packet.
The destination remains latched until software changes it.

To receive, software waits for `STATUS.RX_VALID` and reads `RX_DATA`. That
read consumes one payload. The guest helper in
[`../../../platform/mesh-nic.h`](../../../platform/mesh-nic.h) implements
these two polling sequences.

The wire payload is an uninterpreted 32-bit value. A `float` may be sent by
preserving its bit representation, but the NIC performs no numeric
conversion.

## QEMU-to-SST bridge

The device has one signed 32-bit QOM property, `bridge-fd`, whose default is
`-1`. For an SST-owned QEMU process, Mittens duplicates that tile's bridge
descriptor to child file descriptor 42 and starts QEMU with:

```text
-global mittens-nic.bridge-fd=42
```

During realization the NIC:

1. checks that the file is at least `sizeof(MittensBridgeShared)`;
2. maps the shared structure read/write;
3. closes the inherited descriptor; and
4. validates the ABI magic, version, structure size, and queue capacity.

An incompatible bridge prevents the QEMU device from realizing. The mapping
is released when the device is unrealized.

When no bridge descriptor is provided, the NIC still exists on the `virt`
machine but is disconnected. `STATUS` reads zero, so well-behaved polling
guest code waits indefinitely. Attempting to use `TX_DATA` or `RX_DATA` logs
a QEMU guest error.

## Queue behavior and errors

`TX_READY` reflects available space in the QEMU-to-SST ring. `RX_VALID`
reflects an entry in the SST-to-QEMU ring. The shared rings have 64 entries,
but software must rely on the status bits rather than the current capacity.

Invalid operations are logged with QEMU's `LOG_GUEST_ERROR`. If a bridge is
connected, the first error is also recorded atomically in its
`protocol_error` field:

| Condition | Bridge error |
| --- | --- |
| `TX_DATA` written while no transmit slot is available | `TX_FULL` |
| `RX_DATA` read while no payload is available | `RX_EMPTY` |
| Wrong width, unaligned access, read of a write-only register, or write of a read-only register | `BAD_MMIO` |

Mittens checks this field while servicing the bridge and treats a nonzero
error as fatal. An invalid destination endpoint is rejected by Mittens after
it removes the packet from the transmit ring.

## QEMU source integration

This directory is the project-owned source of the device. QEMU remains a
pinned, unmodified submodule; [`../../../build-scripts/prepare-qemu.sh`](../../../build-scripts/prepare-qemu.sh)
creates a worktree, installs the overlay files, and applies the integration
patches.

| Project file | QEMU worktree destination |
| --- | --- |
| `mittens_nic.c` | `hw/misc/mittens_nic.c` |
| `mittens_nic.h` | `include/hw/misc/mittens_nic.h` |

The patches register the source with QEMU's build system and instantiate one
NIC at `0x10010000` on every RISC-V `virt` machine. Any change to the shared
structure must stay synchronized with
[`../../../bridge/include/mittens/bridge.h`](../../../bridge/include/mittens/bridge.h)
and requires the ABI/version considerations described in the bridge README.
