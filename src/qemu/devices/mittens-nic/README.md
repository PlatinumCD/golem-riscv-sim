# Mittens QEMU NIC

The Mittens NIC is the guest-visible boundary between a bare-metal RISC-V
program running in QEMU and the SST mesh. It is a QEMU `SysBusDevice` with
type name `mittens-nic`, attached to the RISC-V `virt` machine at
`0x10010000`. The device exposes a 4 KiB MMIO window and moves packets through
the shared host bridge documented in
[`../../../bridge/README.md`](../../../bridge/README.md).

The NIC is deliberately small. It does not compute routes, know tile
coordinates, or model network timing. The receive path preserves the SST
source endpoint so runtime software can independently reassemble interleaved
32-bit streams.
See the complete data path in
[`../../../../docs/architecture.md`](../../../../docs/architecture.md).

## Guest-visible registers

All defined accesses are aligned, little-endian, 32-bit operations. The
register interface is also part of the Platform v0.1 contract in
[`../../../../docs/platform-v0.1.md`](../../../../docs/platform-v0.1.md).

| Offset | Name | Access | Meaning |
| ---: | --- | --- | --- |
| `0x00` | `STATUS` | Read | Bit 0 is `TX_READY`; bit 1 is `RX_VALID`; bit 2 is `TX_BURST_READY` |
| `0x04` | `TX_DEST` | Write | Latches the destination endpoint ID |
| `0x08` | `TX_DATA` | Write | Enqueues this payload with the latched destination |
| `0x0c` | `RX_DATA` | Read | Removes and returns the next received payload |
| `0x10` | `RX_SOURCE` | Read | Returns the source of the next payload without consuming it |
| `0x14` | `TX_BURST_ADDRESS_LOW` | Write | Low 32 bits of the guest physical source address |
| `0x18` | `TX_BURST_ADDRESS_HIGH` | Write | High 32 bits of the guest physical source address |
| `0x1c` | `TX_BURST_WORD_COUNT` | Write | Number of 32-bit words, from 1 through 4096 |
| `0x20` | `TX_BURST_SUBMIT` | Write | Snapshots and enqueues the described burst |
| `0x24` | `RX_WAIT` | Write | Yield until receive data is available |
| `0x28` | `TRACE_TASK_ID` | Write | Latches a diagnostic global task ID |
| `0x2c` | `TRACE_EXECUTION_ID_LOW` | Write | Latches diagnostic execution ID bits 31:0 |
| `0x30` | `TRACE_EXECUTION_ID_HIGH` | Write | Latches diagnostic execution ID bits 63:32 |
| `0x34` | `TRACE_EVENT` | Write | Emits task start (`1`), task finish (`2`), memory-init complete (`3`), or a blocking epoch arrival (`4`) |
| `0x38` | `RX_DMA_SOURCE` | Write | Latches the expected source tile |
| `0x3c` | `RX_DMA_ROUTE_ID` | Write | Latches the completion route ID |
| `0x40` | `RX_DMA_ADDRESS_LOW` | Write | Low 32 bits of the guest destination address |
| `0x44` | `RX_DMA_ADDRESS_HIGH` | Write | High 32 bits of the guest destination address |
| `0x48` | `RX_DMA_WORD_COUNT` | Write | Exact number of expected payload words |
| `0x4c` | `RX_DMA_SUBMIT` | Write | Registers the described receive operation |
| `0x50` | `RX_DMA_STATUS` | Read | Bit 0 is submit-ready; bit 1 is completion-valid |
| `0x54` | `RX_DMA_COMPLETION_SOURCE` | Read | Source of the oldest completion |
| `0x58` | `RX_DMA_COMPLETION_ROUTE_ID` | Read | Route ID of the oldest completion |
| `0x5c` | `RX_DMA_COMPLETION_ACK` | Write | Consumes the oldest completion |
| `0x60`-`0xfff` | Reserved | — | Reads return zero; writes are ignored |

To transmit, software waits for `STATUS.TX_READY`, writes `TX_DEST`, executes
an I/O memory fence, and writes the 32-bit payload to `TX_DATA`. The
`TX_DATA` write is the doorbell: each write creates exactly one bridge packet.
The destination remains latched until software changes it.

With managed execution, QEMU buffers packets in the 64-entry transmit ring.
When the ring becomes full, QEMU yields `NIC_TRANSMIT` through fd 41 and SST
drains the batch. A partially filled ring is drained at the next ordinary
fd-41 synchronization boundary. SST still injects every entry as a separate
32-bit network packet; batching changes only the number of host
QEMU-to-SST handshakes, not the simulated wire width or router/link
serialization.

The deployment runtime uses the burst interface. It waits for
`TX_BURST_READY`, writes the destination, guest address, and word count,
executes a memory/I/O fence, and writes `TX_BURST_SUBMIT`. QEMU snapshots at
most 4096 words from guest memory into one of four shared burst slots. SST
submits that slot as one bounded Merlin cell whose `size_in_bits` records the
useful payload. With the default 32-bit, 1 GHz SST link, the 16 KiB cell
occupies the link for 4096 ns. The SST topology may configure a wider physical
link while retaining the same ordered 32-bit words; for example, 64 bits at
1 GHz charges the cell 2048 cycles. A partial cell is conservatively padded
for timing. The bulk path changes host event granularity, not the configured
channel bandwidth.

To receive with source identity, software checks `STATUS.RX_VALID`. If it is
clear, software executes an I/O fence and writes `RX_WAIT`. Under SST-managed
execution, that write yields `NIC_RECEIVE_WAIT` over fd 41 and the tile
remains stopped until its receive bridge contains data. QEMU rechecks both
receive rings in the `RX_WAIT` MMIO operation, closing the race between the
status check and the wait request. If data arrived in that interval, the
write returns without yielding.

Once `RX_VALID` is set, software reads `RX_SOURCE`, executes an I/O fence, and
reads `RX_DATA`. Only the `RX_DATA` read consumes the entry, so both values
describe the same packet. The guest helper in
[`../../../platform/mesh-nic.h`](../../../platform/devices/mesh-nic.h) implements
the payload-only and source-aware blocking sequences. When QEMU is not under
SST-managed execution, `RX_WAIT` returns immediately and the same helper
continues polling, so direct QEMU use remains functional.

The deployment runtime reads each five-word frame header with that legacy
interface, then registers the tensor payload through the receive-DMA
registers. QEMU matches a descriptor by source tile and yields its source,
route ID, and word count to SST through fd 41. Complete fd-42 receive bursts
remain hidden until the tile-local SST DMA engine authorizes them. QEMU then
copies the authorized head burst into the guest physical destination and
publishes a source-and-route completion after the exact requested word count
has arrived. A descriptor may span multiple 4096-word bursts, and up to 64
active descriptors and queued completions are supported in total. Different
sources can be registered independently; a second active descriptor for the
same source is rejected.

This is a destination-side data-movement optimization, not a network bypass.
SST routes and times every architectural 32-bit word, then separately charges
the configured tile-local receive-DMA setup and transfer cycles. A burst is
not guest-visible before both stages complete. QEMU's final host memory copy
is functional and occurs only after SST authorization. Programs without
receive-DMA callbacks continue to consume payloads one word at a time.

The wire payload is an uninterpreted 32-bit value. A `float` may be sent by
preserving its bit representation, but the NIC performs no numeric
conversion.

The four trace registers are control-plane diagnostics. For task events, a
guest writes the task ID and both halves of the 64-bit execution ID, executes
an I/O fence, and writes `TRACE_EVENT`. Event `4` reuses those latches for the
completed epoch ID and the work-complete (`0`) or idle (`1`) contribution.
Under managed execution QEMU yields an fd-41 marker carrying those values;
the epoch marker remains blocked until SST delivers the matching modeled
deployment release. No trace or epoch metadata is injected into fd 42 or the
mesh.

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
machine but is disconnected. `STATUS` reads zero and `RX_WAIT` returns
immediately, so well-behaved guest code waits indefinitely. Attempting to use
`TX_DATA` or `RX_DATA` logs a QEMU guest error.

## Queue behavior and errors

`TX_READY` reflects available space in the legacy word ring.
`TX_BURST_READY` reflects one of four available 4096-word burst slots.
`RX_VALID` reflects data in either receive ring. Software must rely on the
status bits rather than a particular queue capacity.

Invalid operations are logged with QEMU's `LOG_GUEST_ERROR`. If a bridge is
connected, the first error is also recorded atomically in its
`protocol_error` field:

| Condition | Bridge error |
| --- | --- |
| `TX_DATA` written while no transmit slot is available | `TX_FULL` |
| `RX_DATA` or `RX_SOURCE` read while no payload is available | `RX_EMPTY` |
| Wrong width, unaligned access, read of a write-only register, or write of a read-only register | `BAD_MMIO` |
| `TRACE_EVENT` written with a value other than 1 or 2 | `BAD_MMIO` |
| Invalid burst address/count or submission without a free slot | `BAD_BURST` |
| Invalid receive-DMA address/count/source state, size mismatch, full completion queue, or failed guest-memory transaction | `DMA` |
| QEMU cannot snapshot the guest transmit source range | `DMA` |

Mittens checks this field while servicing the bridge and treats a nonzero
error as fatal. An invalid destination endpoint is rejected by Mittens after
it removes the packet from the transmit ring.

## QEMU source integration

This directory is the project-owned source of the device. QEMU remains a
pinned, unmodified submodule; [`../../../../build-scripts/prepare-qemu.sh`](../../../../build-scripts/prepare-qemu.sh)
creates a worktree, installs the overlay files, and applies the integration
patches.

| Project file | QEMU worktree destination |
| --- | --- |
| `mittens_nic.c` | `hw/misc/mittens_nic.c` |
| `mittens_nic.h` | `include/hw/misc/mittens_nic.h` |

The patches register the source with QEMU's build system and instantiate one
NIC at `0x10010000` on every RISC-V `virt` machine. Any change to the shared
structure must stay synchronized with
[`../../../bridge/include/mittens/NICTileBridge.h`](../../../bridge/include/mittens/NICTileBridge.h)
and requires the ABI/version considerations described in the bridge README.
