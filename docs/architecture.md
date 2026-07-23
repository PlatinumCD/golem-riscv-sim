# System architecture

This document describes how the compiler, bare-metal tile software, QEMU,
Mittens, and SST Merlin form the current Platform v0 system.

## Component boundary

Each simulated tile is one independent QEMU system-emulation process with one
RISC-V hart and 16 MiB of private guest RAM. An SST `mittens.tile` component
owns that process and connects it to one endpoint of the Merlin network.

```text
bare-metal RISC-V ELF
        |
        | loads at 0x80000000
        v
QEMU riscv64 virt machine
  +---------------------------+
  | one hart                  |
  | private 16 MiB RAM        |
  | UART and test/exit device |
  | mittens-nic MMIO device   |
  +---------------------------+
        |
        | per-tile memfd with two SPSC queues
        v
SST mittens.tile
        |
        | SST::Interfaces::SimpleNetwork
        v
merlin.linkcontrol
        |
        v
merlin.hr_router <----> neighboring Merlin routers
```

The guest-visible platform contract is defined in
[`platform-v0.md`](platform-v0.md). The shared host bridge is an implementation
detail documented in [`../bridge/README.md`](../bridge/README.md).

## Responsibility and ownership

| Component | Owns | Does not own |
| --- | --- | --- |
| Bare-metal ELF | Tile computation, MMIO operations, application protocol | Routing, host process lifecycle |
| QEMU | RISC-V instruction semantics, private RAM, local devices | Mesh topology or SST simulated time |
| `mittens-nic` | Guest MMIO state and QEMU side of the shared queues | Destination routing |
| `mittens.tile` | QEMU lifecycle, bridge, SST endpoint conversion | RISC-V execution |
| Merlin | Router topology, buffering, link bandwidth, latency, backpressure | Guest instruction timing |
| SST Core | Discrete-event schedule and component lifecycle | QEMU instruction progress |

Guest RAM is never shared between tiles. A tile can affect another tile only
by sending a packet through its NIC and the SST network.

## Build-time composition

Pinned upstream submodules remain unmodified. The build creates detached
worktrees and overlays project-owned integration code:

```text
third_party/qemu
  + components/devices/mittens-nic
  + bridge/include/mittens/bridge.h
  + patches/qemu
      -> build/sources/qemu

third_party/sst-elements
  + components/elements/mittens
  + bridge/include/mittens/bridge.h
      -> build/sources/sst-elements
```

The QEMU patches register the `mittens-nic` device and attach one instance to
the RISC-V `virt` machine at `0x10010000`. The SST Elements preparation enables
only Merlin and Mittens.

Bare-metal tests are linked separately for each tile. This permits a tile ELF
to contain only its local computation and runtime state; routine code is not
transferred over the mesh.

## Tile startup and shutdown

For a network-attached managed tile, `mittens.tile` performs this sequence:

1. Configure the `SST::Interfaces::SimpleNetwork` subcomponent.
2. Create and initialize a per-tile anonymous `memfd` bridge.
3. Map that bridge into SST.
4. Duplicate its file descriptor to descriptor 42 in the QEMU child.
5. Launch QEMU with the tile ELF and the `mittens-nic.bridge-fd=42` property.
6. Poll the child process and bridge from an SST clock handler.
7. Treat a nonzero or signal-based QEMU exit as a fatal simulation error.
8. Terminate and reap any surviving child during normal or emergency cleanup.

The effective QEMU command for an attached tile is:

```text
qemu-system-riscv64 \
  -machine virt -smp 1 -m 16M \
  -bios none -kernel <tile.elf> \
  -display none -monitor none -serial stdio -no-reboot \
  -global mittens-nic.bridge-fd=42
```

The child maps the bridge during device realization, validates its ABI header,
and closes its copy of the file descriptor after `mmap`. Both processes retain
their mappings until tile teardown.

## Packet data path

A guest transmission follows this sequence:

1. The tile waits for `STATUS.TX_READY`.
2. It writes the final endpoint ID to `TX_DESTINATION`.
3. It writes a 32-bit payload to `TX_DATA`, which is the transmit doorbell.
4. QEMU appends `{destination, payload}` to the bridge transmit ring.
5. `mittens.tile` removes the packet during a bridge poll.
6. It creates a 32-bit SST network request whose source and destination are
   endpoint IDs and whose `PacketEvent` contains the 32-bit payload.
7. Merlin selects router ports, applies buffering and link timing, and delivers
   the request to the destination endpoint.
8. The destination `mittens.tile` places the payload in its bridge receive ring.
9. The destination QEMU exposes `STATUS.RX_VALID`; reading `RX_DATA` consumes
   the oldest payload.

The SST request retains the source endpoint for diagnostics, but Platform v0
does not expose that source ID to the receiving guest.

## Mesh identity and routing

Endpoint IDs are zero-based and row-major:

```text
tile_id = y * mesh_width + x
```

Each router stores its own coordinates when the SST graph is constructed.
Merlin derives destination coordinates when a packet enters the mesh, then
uses deterministic X-then-Y routing. Intermediate QEMU processes and tile
programs never receive transit packets; only their local routers forward them.

The reusable test topology is implemented in
[`../tests/support/mesh.py`](../tests/support/mesh.py).

## Current architectural limits

- Guest programs poll the NIC; Platform v0 does not use NIC interrupts.
- The bridge is polled rather than event-notified.
- Receive packets expose only a payload, not their source endpoint.
- Each current QEMU NIC transaction carries only a destination endpoint and
  one 32-bit payload. There is no multiword network transaction or runtime
  header. The distributed matvec receiver uses its compiled task order and
  fixed four-word input size.
- There is no operating system, dynamic loader, pthread runtime, or OpenMP
  runtime in a tile.
- QEMU instruction execution is not synchronized with SST simulated time.

The last limitation determines which timing measurements are meaningful. See
[`timing-model.md`](timing-model.md) before interpreting simulation times or
cycle counts.
