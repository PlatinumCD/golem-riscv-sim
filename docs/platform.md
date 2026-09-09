# Golem platform

This document describes the platform software can use on one simulated tile.
It is a current interface description, not a history of platform revisions.

## Tile

Each tile contains one RV64 RISC-V hart running a bare-metal ELF, private
guest RAM, optional scratchpad memory, a UART, a simulation-exit device, a
mesh NIC, and optional DMA and analog devices.

Tiles do not share guest RAM. Tile-to-tile communication uses the mesh NIC.
Global RAM is a separate explicitly modeled path.

QEMU owns functional instruction execution and guest bytes. SST controls
simulated time and models CPU issue, scratchpad service, DMA, network traffic,
queueing, and accelerator timing.

## Guest address map

| Address | Size | Interface |
|---|---:|---|
| 0x00100000 | 4 KiB | QEMU test/exit device |
| 0x10000000 | 4 KiB | UART |
| 0x10010000 | 4 KiB | Mesh NIC |
| 0x10011000 | 4 KiB | Scratchpad/global-DMA registers |
| 0x80000000 | configured RAM size | Tile-private guest RAM |
| 0x90000000 | configured SPM capacity | Tile-private scratchpad window |

The scratchpad base is fixed at 0x90000000; its capacity is configurable.
Ordinary program data lives in private RAM unless software explicitly uses
the scratchpad.

## Scratchpad

The scratchpad is software-managed, private, and noncoherent. A buffer is a
range of addresses inside the scratchpad. It is not a cache.

Current defaults are:

| Parameter | Default |
|---|---:|
| Capacity | 256 KiB |
| Banks | 8 |
| Read ports per bank | 1 |
| Write ports per bank | 1 |
| Access width | 256 bits / 32 bytes |
| Completion latency | 1 CPU cycle |
| DMA service rate | 32 bytes per CPU cycle |
| DMA setup | 8 CPU cycles |

The generic tile configuration disables the scratchpad by default; workloads
enable it when needed.

With 32-byte stripes and eight banks:

    bank = (scratchpad_offset / 32) mod 8

Offsets 0 through 224 in 32-byte steps select banks 0 through 7. Offset 256
selects bank 0 again.

CPU/RVV accesses, TX DMA, RX DMA, and global-memory DMA use the same timing
model. Different banks can serve independent requests concurrently. Read and
write ports are separate. Requests competing for one port wait.

A full-width RVV e32,m1 load or store transfers eight 32-bit values, or
32 bytes. The timing path preserves that aligned operation as one full-width
SPM transaction.

## Mesh NIC and DMA

The NIC is a programmed-I/O device at 0x10010000. Scalar sends enqueue one
32-bit payload. Burst sends snapshot up to 4096 32-bit words from guest
memory. SST still models transfer service, network transmission, buffering,
and backpressure.

Receive DMA registers a source tile, route, destination address, and exact
word count. SST authorizes each received burst after modeled local DMA work;
QEMU then copies the authorized data into guest memory.

TX and RX stream counts are independent. Each supports 1, 2, or 4 lanes; the
default is one lane. Lanes share existing scratchpad banks, physical links,
and router capacity. More lanes expose scheduling concurrency; they do not
multiply hardware bandwidth.

TX reads source data through the scratchpad timing model into bounded FIFO
state. If the network cannot consume data, the FIFO fills and TX prefetch
stops. RX schedules writes through the common scratchpad arbiter. RX queue
depth is separate from RX lane count.

Host descriptors 41, 42, and 43 carry synchronization, network data, and
analog data between QEMU and SST. They are not physical network links.

## Mesh communication

Tiles form a rectangular mesh with row-major IDs:

    tile_id = y * mesh_width + x

The Mittens wormhole router normally uses deterministic XY routing: X first,
then Y. A packet reserves its output until its tail departs. Finite buffers
and credits apply backpressure. Competing packet heads use round-robin
arbitration.

The physical link width is configurable. The default is 32 bits, or 4 bytes
per link cycle at a 1 GHz link clock.

Keep these limits separate:

- per-tile injection and ejection;
- one directed physical link; and
- aggregate service across independent links.

Flows on different physical links can overlap. Flows requesting the same
output serialize. Distance adds route and pipeline delay; packet
serialization and contention determine link occupancy.

## Global RAM and synchronization

The scratchpad DMA register block supports global-RAM-to-SPM and
SPM-to-global-RAM requests, single waits, bounded wait batches, and
compiler-certified macro boundaries. Global RAM is a separate modeled
resource.

Initialization and completion barriers are sideband SST controllers. They
coordinate tiles but are not payload packets on the mesh.

## Analog instructions

Optional analog arrays are accessed through mvm.set, mvm.l, mvm, mvm.s, and
mvm.mv.

Each array has an ordered command queue and independent compute state, but all
arrays share one bidirectional 256-bit tile-local analog link. The link
transfers one 32-byte beat per cycle and is separate from the digital mesh.

mvm.set can encode a compact valid matrix shape; QEMU snapshots the valid
rectangle and SST expands it to the configured physical geometry. mvm.s waits
for the selected output before copying it into guest memory. Native and
CrossSim provide numerical behavior; host backend time is not simulated
hardware time.

## Timing boundary

QEMU executes RV64/RVV semantics. SST models issue cycles as:

    max(ceil(retired_instructions / cpu_issue_width),
        retired_vector_instructions)

The scalar issue width is 1, 2, or 4; vector issue is limited to one
instruction per CPU cycle. This is an issue-accounting model, not a detailed
out-of-order CPU pipeline.

Host batching changes host overhead, not modeled hardware capacity.
Service cycles, queue cycles, and stall counters may overlap. They must not
be summed and reported as elapsed runtime. Missing counters remain missing;
they are not interpreted as measured zero.

For implementation details, see architecture.md, vector-architecture.md,
analog-isa.md, and timing-model.md.
