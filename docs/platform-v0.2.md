# Golem Tile Platform v0.2

This document defines the optional scratchpad extension for the Golem tile.
Platform v0.2 includes all Platform v0.1 requirements.

The words `must` and `must not` identify requirements in this document.

## Compatibility

The scratchpad extension is optional. A workspace-only ELF does not require
the extension. The simulator uses the Platform v0.1 behavior when the
extension is disabled.

The backing-memory selection remains `native` or `memhierarchy`. The
scratchpad is a separate local-storage tier. It does not replace the selected
backing memory.

## Address map additions

| Start | End | Maximum size | Device or region |
| ---: | ---: | ---: | --- |
| `0x10011000` | `0x10011fff` | 4 KiB | Scratchpad DMA MMIO |
| `0x90000000` | `0x90ffffff` | 16 MiB | Tile-private scratchpad window |

Each tile owns an independent scratchpad window. The configured scratchpad
size can be less than 16 MiB. An access outside the configured size is an
error.

The scratchpad is private and noncoherent. Another tile must use the mesh NIC
to transfer a resource to or from this scratchpad.

## Configuration record

The compiler and simulator use the same fields and units.

| Field | Unit | Initial default |
| --- | --- | ---: |
| `scratchpad-enabled` | Boolean | `false` |
| `scratchpad-bytes` | bytes | 262144 |
| `scratchpad-banks` | count | 8 |
| `scratchpad-read-ports` | ports per bank | 1 |
| `scratchpad-write-ports` | ports per bank | 1 |
| `scratchpad-access-width-bits` | bits per port cycle | 256 |
| `scratchpad-latency-cycles` | scratchpad cycles | 1 |
| `scratchpad-queue-entries` | requests | 16 |
| `dma-bytes-per-cycle` | bytes per scratchpad cycle | 32 |
| `dma-setup-cycles` | scratchpad cycles | 8 |
| `dma-queue-entries` | descriptors | 8 |

The compiler records offsets. It must not record absolute scratchpad
addresses. The runtime calculates `0x90000000 + offset`.

The deployment launcher must compare the compiler record with the simulator
configuration. It must stop if the compiler requirement exceeds a simulator
limit.

## Compiler option

The exact allocation pass owns the local-memory option:

```text
--sculptor-plan-core-scratchpad="local-memory=scratchpad bytes=N alignment=N double-buffer-boundaries=true"
```

The legacy pipeline omits this pass. The value `local-memory=workspace` is a
no-op form for compiler tests.

The exact allocation pass runs after core extraction. Global analysis can use
the scratchpad parameters before scheduling.

## Resource ABI

The existing `golem::runtime::Resource` record remains 48 bytes. Platform
v0.2 adds these flag values:

| Flag | Value | Meaning |
| --- | ---: | --- |
| `ResourceWorkspace` | `1 << 0` | The resource uses workspace storage |
| `ResourceExternal` | `1 << 1` | The resource uses external storage |
| `ResourceScratchpad` | `1 << 2` | The resource uses scratchpad storage |
| `ResourceSpill` | `1 << 3` | The compiler moved the resource to backing storage |

Exactly one primary storage flag must be set. The primary flags are
`ResourceWorkspace`, `ResourceExternal`, and `ResourceScratchpad`.

For a scratchpad resource, `workspace_offset` contains the scratchpad byte
offset. This use preserves the 48-byte record layout.

The `ResourceSpill` flag can occur with `ResourceWorkspace`. It must not occur
with `ResourceExternal` or `ResourceScratchpad`.

## Tile feature exports

A scratchpad ELF exports these functions:

```cpp
extern "C" uint32_t golem_tile_abi_features();
extern "C" uint64_t golem_tile_scratchpad_required_bytes();
extern "C" const ScratchpadDmaDescriptor* golem_tile_dma_descriptors();
extern "C" uint32_t golem_tile_dma_descriptor_count();
```

Bit zero of `golem_tile_abi_features()` identifies the scratchpad DMA
extension. The runtime supplies zero-value weak defaults for an old ELF.

The runtime must stop if an ELF requires an unsupported feature. The runtime
must also stop if the required byte count exceeds the configured capacity.

## DMA descriptor wire format

The descriptor uses little-endian fixed-width fields. Its alignment is 8
bytes. Its total size is 64 bytes.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `uint32_t` | `descriptor_id` |
| 4 | `uint32_t` | `direction` |
| 8 | `uint32_t` | `local_slot` |
| 12 | `uint32_t` | `route_id` |
| 16 | `uint64_t` | `scratchpad_offset` |
| 24 | `uint64_t` | `byte_size` |
| 32 | `uint32_t` | `completion_token_id` |
| 36 | `uint32_t` | `trigger_kind` |
| 40 | `uint32_t` | `trigger_id` |
| 44 | `uint32_t` | `flags` |
| 48 | `uint32_t` | `source_storage` |
| 52 | `uint32_t` | `destination_storage` |
| 56 | `uint64_t` | `reserved` |

The `reserved` field must be zero. `UINT32_MAX` identifies an inapplicable
route ID or trigger ID. `UINT64_MAX` identifies an inapplicable offset.

### Direction values

| Name | Value |
| --- | ---: |
| `BackingToScratchpad` | 0 |
| `ScratchpadToBacking` | 1 |
| `NicToScratchpad` | 2 |
| `ScratchpadToNic` | 3 |

### Storage values

| Name | Value |
| --- | ---: |
| `Backing` | 0 |
| `Scratchpad` | 1 |
| `Nic` | 2 |

### Trigger values

| Name | Value | `trigger_id` namespace |
| --- | ---: | --- |
| `Boot` | 0 | `UINT32_MAX` |
| `ResourceReady` | 1 | Global resource ID |
| `RouteArrival` | 2 | Route ID |
| `TaskComplete` | 3 | Global task ID |

The compiler must use the preserved deployment-level global IDs. It must not
use a core-local resource slot or a core-local runtime task index in
`trigger_id`.

### Flag values

| Name | Value |
| --- | ---: |
| `Asynchronous` | `1 << 0` |
| `Blocking` | `1 << 1` |
| `PingPongZero` | `1 << 2` |
| `PingPongOne` | `1 << 3` |

Exactly one of `Asynchronous` and `Blocking` must be set. The first compiler
implementation uses `Asynchronous`.

The runtime keys completion state by `(execution_id,
completion_token_id)`. A static token ID does not identify an execution.

## DMA MMIO registers

All registers are little-endian 32-bit registers. All accesses must be aligned
to 4 bytes. Offsets are relative to `0x10011000`.

| Offset | Name | Access | Meaning |
| ---: | --- | --- | --- |
| `0x00` | `SOURCE_LO` | Read/write | Source address bits 31 through 0 |
| `0x04` | `SOURCE_HI` | Read/write | Source address bits 63 through 32 |
| `0x08` | `DESTINATION_LO` | Read/write | Destination address bits 31 through 0 |
| `0x0c` | `DESTINATION_HI` | Read/write | Destination address bits 63 through 32 |
| `0x10` | `BYTE_COUNT` | Read/write | Transfer size in bytes |
| `0x14` | `TOKEN_ID` | Read/write | Local completion token ID |
| `0x18` | `EXECUTION_LO` | Read/write | Execution ID bits 31 through 0 |
| `0x1c` | `EXECUTION_HI` | Read/write | Execution ID bits 63 through 32 |
| `0x20` | `DIRECTION` | Read/write | DMA direction value |
| `0x24` | `COMMAND` | Write | Submit or wait command |
| `0x28` | `STATUS` | Read | Device status bits |
| `0x2c` | `ERROR` | Read | Device error code |

The `COMMAND` value 1 submits a descriptor. The `COMMAND` value 2 waits for
the selected execution ID and token ID.

The `STATUS` bit zero is `READY`. Bit one is `COMPLETE`. Bit two is `ERROR`.
Bit three is `BUSY`.

## Synchronization events

Platform v0.2 adds two fd-41 stop reasons:

| Name | Value |
| --- | ---: |
| `SCRATCHPAD_DMA_SUBMIT` | 13 |
| `SCRATCHPAD_DMA_WAIT` | 14 |

A submit event stops QEMU until SST accepts or rejects the descriptor. An
accepted asynchronous descriptor does not wait for transfer completion.

A wait event stops QEMU until SST completes the selected descriptor. QEMU
commits the staged destination bytes after SST resumes the wait event.

The destination resource is not ready before this commit. Tasks cannot read a
partially transferred resource in Platform v0.2.

## Timing rules

CPU and DMA requests use the same scratchpad banks and ports. A DMA transfer
is not a free side path.

The initial arbiter gives accepted DMA reservations priority over later CPU
requests. The simulator reports this policy in its configuration output.

The mesh still transfers 32 bits on each link transfer. Resource-level DMA
completion does not change the mesh link width.

## Required first test

The first joint test uses one two-task producer-consumer chain.

The workspace case uses the current materialized intermediate. The scratchpad
case uses one input DMA and one output DMA. The internal intermediate remains
in the scratchpad.

The test must compare the complete output tensors. It must report maximum
absolute error, maximum relative error, RMSE, and the mismatch count.

The test must also report these timing values:

- Backing-memory requests.
- Scratchpad requests and bank conflicts.
- DMA bytes, queue time, and overlap.
- Peak scratchpad occupancy.
- Critical-path memory stall.
- Aggregate memory latency.
- End-to-end simulated time.
