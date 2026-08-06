# Mittens host bridge ABI

Mittens uses three per-tile shared-memory bridges with one control plane:

| QEMU descriptor | Header | Purpose |
| ---: | --- | --- |
| 41 | `SyncTileBridge.h` | Instruction grants, yields, stop reasons, and resume |
| 42 | `NICTileBridge.h` | Mesh transmit and receive packet data |
| 43 | `AnalogTileBridge.h` | Analog commands, operands, and results |

Descriptors 42 and 43 never grant execution or resume a hart. Every device
boundary is reported through fd 41.

[`include/mittens/SyncTileBridge.h`](include/mittens/SyncTileBridge.h) is the
fixed 128-byte control contract. SST publishes a grant epoch and instruction
budget, QEMU runs under precise icount, and QEMU publishes a monotonic executed
count plus a stop reason. Analog events also identify their array channel and
slot sequence. Task trace events identify the global task ID and execution ID.
Receive-DMA submission events identify the source tile, route ID, and exact
word count. Optional memory events identify a physical address, byte size, and
read/write direction. They carry no guest data: QEMU remains the functional
RAM owner while SST models completion through StandardMem.
ABI version 6 introduced `MEMORY_INIT_COMPLETE`. Before that explicit guest
marker, QEMU may aggregate initialization accesses into access, read-byte, and
write-byte counters. It then performs one fd 41 handshake so SST can charge
the complete initialization transfer without one host synchronization per
access. After SST resumes the hart, ordinary `MEMORY_ACCESS` events use the
detailed StandardMem path again.
ABI version 7 adds the burst/scalar NIC event discriminator and
`NIC_TRANSMIT_WAIT`. Every accepted deployment burst publishes a zero-wait
`NIC_TRANSMIT` descriptor doorbell, so SST observes the exact CPU timestamp at
which the fd 42 descriptor became visible. A guest that finds the burst ring
full writes `TX_WAIT`; SST holds that hart only until a burst slot is free.
Release/acquire atomics publish state transitions; shared futex wakeups
implement the fd 41 grant/yield handshake without polling.
`GUEST_EXIT` is a terminal event: QEMU publishes it before normal finisher
shutdown, and SST schedules and reaps the child at that instruction boundary.
The current synchronization ABI is version 7 and remains exactly 128 bytes.

[`include/mittens/NICTileBridge.h`](include/mittens/NICTileBridge.h) is the versioned
shared-memory contract between one QEMU `mittens-nic` device and one SST
`mittens.tile` component. It is C-compatible so the QEMU C device and SST C++
element compile against the same declarations and inline queue operations.

This ABI is internal to the host integration. It is distinct from the
guest-visible MMIO contract in
[`../docs/platform-v0.1.md`](../docs/platform-v0.1.md).

[`include/mittens/AnalogTileBridge.h`](include/mittens/AnalogTileBridge.h)
defines the separate tile-local analog path. It fixes the operation and status
values, the 24-byte command, the 8-byte response, the 32-byte/256-bit data
beat, and a geometry-sized shared-memory layout. Every array owns an
independent channel with four command slots. QEMU publishes slot data; SST
accepts commands, meters their transfer and compute work, and publishes
completion. Acceptance and completion waits are controlled through fd 41.
The per-array channels preserve ordering and queue ownership; they do not
represent separate physical links. SST arbitrates their payload transfers
over one shared 256-bit link per tile.

Analog-enabled tiles duplicate this second `memfd` to descriptor 43 and pass
`mittens-analog.bridge-fd=43` to QEMU. Its 64-byte header records tile ID,
array count, common rows and columns, queue capacity, payload size, link
width, and dynamic strides. Each array channel separates QEMU's write index
from SST's accept index by cache lines. A slot contains metadata followed by
enough 32-bit payload storage for one complete matrix. Release/acquire atomics
publish data-state transitions. The fd 41 bridge, not the analog slot, resumes
the QEMU hart after acceptance or completion.

## Creation and mapping

For each managed tile, SST first creates the fd 41 synchronization bridge.
It additionally creates fd 42 when a network is attached and fd 43 when analog
arrays are configured. For the NIC data bridge, SST:

1. creates a close-on-exec anonymous `memfd` named
   `mittens-tile-<tile_id>`;
2. resizes it to `sizeof(MittensBridgeShared)`;
3. maps it shared and read/write;
4. zeroes the complete structure and writes the ABI header; and
5. duplicates a staged copy to file descriptor 42 in the QEMU child.

QEMU receives descriptor 42 through the `mittens-nic.bridge-fd` property. The
device checks the file size, maps the structure, closes the descriptor, and
validates the magic, version, structure size, and queue capacity. SST retains
its descriptor and mapping until tile cleanup.

There is one bridge per tile. Bridges are never shared between QEMU tiles.
Staging all active bridge descriptors above the fixed 41-43 range prevents
one `dup2` operation from overwriting the source of a later mapping.

## NIC ABI constants

| Field | Version 4 value |
| --- | ---: |
| Magic | `0x4d495454` |
| Version | `4` |
| Queue capacity | 64 entries per direction |
| Burst queue capacity | 4 entries per direction |
| Burst payload capacity | 4096 32-bit words |
| Packet size | 8 bytes |
| Complete structure size | 132,800 bytes (`0x206c0`) |
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
| `0x2c0` | 640 B | SST-to-QEMU receive ring |
| `0x540` | 65,728 B | QEMU-to-SST burst transmit ring |
| `0x10600` | 65,728 B | SST-to-QEMU burst receive ring |

The header fields are:

| Offset | Field | Meaning |
| ---: | --- | --- |
| `0x00` | `magic` | Bridge type identifier |
| `0x04` | `version` | ABI version |
| `0x08` | `structure_size` | Expected mapped structure size |
| `0x0c` | `queue_capacity` | Entries in each ring |
| `0x10` | `tile_id` | SST endpoint that owns this bridge |
| `0x14` | `protocol_error` | First reported bridge protocol error |
| `0x18` | `rx_dma_timing_enabled` | QEMU must wait for SST DMA authorization |
| `0x1c` | `rx_dma_authorization_write_index` | Authorizations published by SST |
| `0x20` | `rx_dma_authorization_read_index` | Authorizations consumed by QEMU |
| `0x24` | padding | Extends the header to 64 bytes |

Transmit entries contain:

```c
typedef struct MittensBridgePacket {
    uint32_t destination;
    uint32_t payload;
} MittensBridgePacket;
```

Receive entries contain the 32-bit payload and SST source endpoint. Version 4
also contains four-slot transmit and receive burst rings. Each burst holds a
source or destination, a word count from 1 through 4096, and that many 32-bit
words. Its authorization counters prevent QEMU from consuming a receive-DMA
burst before SST's tile-local transfer completes.

## Queue ownership

Each direction is a bounded single-producer/single-consumer queue:

| Ring | Producer | Consumer | Contents |
| --- | --- | --- | --- |
| `transmit` | QEMU NIC | `mittens.tile` | Destination and payload |
| `receive` | `mittens.tile` | QEMU NIC | Source endpoint and payload |
| `burst_transmit` | QEMU NIC | `mittens.tile` | Destination and up to 4096 payload words |
| `burst_receive` | `mittens.tile` | QEMU NIC | Source and up to 4096 payload words |

Indices are monotonic 32-bit counters. The physical slot is
`index % MITTENS_BRIDGE_QUEUE_CAPACITY`; unsigned subtraction of the write and
read counters determines occupancy. A word ring is full when that difference
is 64; a burst ring is full when it is four. A ring is empty when both counters
are equal. The burst receive consumer additionally owns `read_word_offset`,
allowing guest `RX_DATA` reads to consume one 32-bit beat at a time without
generating one SST event per beat.

The receive-DMA path may instead peek and consume the complete head burst.
When timing is enabled, it also requires one unconsumed SST authorization.
That operation is valid only while `read_word_offset` is zero, so a burst
partially consumed through `RX_DATA` can never also be copied by DMA. The
producer slot is released only after QEMU has copied all words into guest
memory.

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

The fd 42 data bridge does not contain locks, timestamps, or CPU instruction
counts. Every accepted deployment burst therefore publishes a zero-wait
`NIC_TRANSMIT` descriptor doorbell through fd 41. SST services fd 42 at that
exact CPU boundary and immediately resumes QEMU unless the four-entry burst
ring remains full. A `TX_WAIT` write closes the status-read/wait race and
holds the hart until SST has freed the requested ring type. The legacy scalar
path continues to yield when its 64-entry ring becomes full.

QEMU yields `NIC_RECEIVE_WAIT` through fd 41 when an `RX_WAIT` write rechecks
both receive rings and finds them empty. fd 42 never wakes a hart by itself:
Mittens observes delivered fd 42 data and performs the matching fd 41 resume.
Receive-DMA descriptor submission is also an fd-41 event. SST schedules
eligible receive bursts on its local DMA clock and increments the
authorization write index only at completion; QEMU consumes the matching
authorization before releasing that burst slot. SST services a reported
boundary only after its instruction count reaches the scheduled CPU cycle.
See [`../docs/timing-model.md`](../docs/timing-model.md).

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
| 5 | `BAD_BURST` | Invalid burst size or submission |
| 6 | `DMA` | QEMU could not snapshot the specified guest memory |

`mittens.tile` checks the error field during bridge service and turns any
nonzero value into a fatal SST diagnostic. Invalid endpoint IDs are detected
when Mittens removes a transmit entry and are also fatal.
