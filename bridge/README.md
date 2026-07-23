# Mittens host bridge ABI

[`include/mittens/bridge.h`](include/mittens/bridge.h) is the versioned
shared-memory contract between one QEMU `mittens-nic` device and one SST
`mittens.tile` component. It is C-compatible so the QEMU C device and SST C++
element compile against the same declarations and inline queue operations.

This ABI is internal to the host integration. It is distinct from the
guest-visible MMIO contract in
[`../docs/platform-v0.md`](../docs/platform-v0.md).

## Creation and mapping

For each network-attached tile, SST:

1. creates an anonymous `memfd` named `mittens-tile-<tile_id>`;
2. resizes it to `sizeof(MittensBridgeShared)`;
3. maps it shared and read/write;
4. zeroes the complete structure and writes the ABI header; and
5. duplicates the descriptor to file descriptor 42 in the QEMU child.

QEMU receives descriptor 42 through the `mittens-nic.bridge-fd` property. The
device checks the file size, maps the structure, closes the descriptor, and
validates the magic, version, structure size, and queue capacity. SST retains
its descriptor and mapping until tile cleanup.

There is one bridge per tile. Bridges are never shared between QEMU tiles.

## ABI constants

| Field | Version 1 value |
| --- | ---: |
| Magic | `0x4d495454` |
| Version | `1` |
| Queue capacity | 64 entries per direction |
| Packet size | 8 bytes |
| Complete structure size | 1,088 bytes (`0x440`) |
| Required structure alignment | 64 bytes |

Both sides validate the sizes at compile time where possible. Changing a
field layout, queue capacity, or structure size requires an ABI version change
and corresponding validation updates in QEMU and Mittens.

## Shared-memory layout

The top-level header occupies one cache line. Producer and consumer indices
also occupy separate cache lines to avoid false sharing.

| Offset | Size | Contents |
| ---: | ---: | --- |
| `0x000` | 64 B | ABI header and padding |
| `0x040` | 640 B | QEMU-to-SST transmit ring |
| `0x2c0` | 384 B | SST-to-QEMU receive ring |

The header fields are:

| Offset | Field | Meaning |
| ---: | --- | --- |
| `0x00` | `magic` | Bridge type identifier |
| `0x04` | `version` | ABI version |
| `0x08` | `structure_size` | Expected mapped structure size |
| `0x0c` | `queue_capacity` | Entries in each ring |
| `0x10` | `tile_id` | SST endpoint that owns this bridge |
| `0x14` | `protocol_error` | First reported bridge protocol error |
| `0x18` | padding | Extends the header to 64 bytes |

Transmit entries contain:

```c
typedef struct MittensBridgePacket {
    uint32_t destination;
    uint32_t payload;
} MittensBridgePacket;
```

Receive entries contain only the 32-bit payload. SST knows the request source,
but Platform v0 does not expose it to QEMU or guest software.

## Queue ownership

Each direction is a bounded single-producer/single-consumer queue:

| Ring | Producer | Consumer | Contents |
| --- | --- | --- | --- |
| `transmit` | QEMU NIC | `mittens.tile` | Destination and payload |
| `receive` | `mittens.tile` | QEMU NIC | Payload only |

Indices are monotonic 32-bit counters. The physical slot is
`index % MITTENS_BRIDGE_QUEUE_CAPACITY`; unsigned subtraction of the write and
read counters determines occupancy. A ring is full when that difference is 64
and empty when both counters are equal.

The implementation assumes exactly one producer and one consumer for each
ring. Adding another writer or reader requires a different synchronization
algorithm.

## Memory ordering

The producer writes an entry and then publishes the incremented write index
with release ordering. The consumer observes the write index with acquire
ordering before reading the entry. After consuming it, the consumer publishes
the incremented read index with release ordering; the producer observes that
index with acquire ordering before reusing a slot.

Each side reads its privately owned index with relaxed ordering. The ABI uses
compiler atomic builtins so the C and C++ consumers share identical ordering
semantics.

The bridge does not contain locks, event notifications, timestamps, or CPU
instruction counts. Mittens currently discovers queue changes by polling. See
[`../docs/timing-model.md`](../docs/timing-model.md) for the consequences.

## Protocol errors

`protocol_error` records the first error by compare-and-exchange. Later errors
do not replace the original diagnostic.

| Value | Name | Current meaning |
| ---: | --- | --- |
| 0 | `NONE` | No bridge error |
| 1 | `TX_FULL` | Guest wrote `TX_DATA` when the transmit ring could not accept it |
| 2 | `RX_EMPTY` | Guest read `RX_DATA` without an available payload |
| 3 | `BAD_MMIO` | Invalid width, alignment, or register access direction |
| 4 | `BAD_DESTINATION` | Reserved ABI code; Mittens currently rejects an invalid destination directly |

`mittens.tile` checks the error field during bridge service and turns any
nonzero value into a fatal SST diagnostic. Invalid endpoint IDs are detected
when Mittens removes a transmit entry and are also fatal.
