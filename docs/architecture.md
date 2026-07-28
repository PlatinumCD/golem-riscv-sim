# System architecture

This document describes how the compiler, bare-metal tile software, QEMU,
Mittens, and SST Merlin form the current Platform v0.1 system.

## End-to-end system

The complete verified application path is:

```text
PyTorch model
  → Torch-MLIR
  → Sculptor task extraction, scheduling, fusion, and partitioning
  → one bare-metal RISC-V ELF per tile
  → tile runtime with boot and task registries
  → custom Golem analog ISA
  → QEMU execution
  → SST timing and mesh simulation
  → native or CrossSim analog backend
  → 32-bit routed tensor transfers
```

This path connects model import and compilation to independently bootable tile
programs, analog execution, and cycle-synchronized communication through the
simulated mesh.

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
  | mittens-analog device     |
  +---------------------------+
        |
        | fd 41 control, fd 42 NIC data, fd 43 analog data
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
[`platform-v0.1.md`](platform-v0.1.md). The shared host bridge is an implementation
detail documented in [`../bridge/README.md`](../bridge/README.md).

## Responsibility and ownership

| Component | Owns | Does not own |
| --- | --- | --- |
| Bare-metal ELF | Tile computation, MMIO operations, application protocol | Routing, host process lifecycle |
| QEMU | RISC-V instruction semantics, custom analog decode, private RAM, local devices, precise instruction counting | Mesh topology or authority over simulated time |
| `mittens-nic` | Guest MMIO state and QEMU side of the shared queues | Destination routing |
| `mittens-analog` | Guest-memory snapshots and QEMU side of the per-array queues | Numerical MVM or simulated latency |
| `mittens-sync` | QEMU side of instruction grants, device-boundary yields, and task markers | CPU timing policy |
| `mittens.tile` | QEMU lifecycle, fd 41 control, NIC and analog bridges, SST endpoint conversion, CPU and analog timing, optional task timestamps | RISC-V instruction semantics |
| Merlin | Router topology, buffering, link bandwidth, latency, backpressure | Guest instruction timing |
| SST Core | Discrete-event schedule, component lifecycle, and QEMU execution authority | RISC-V instruction semantics |

Guest RAM is never shared between tiles. A tile can affect another tile only
by sending a packet through its NIC and the SST network.

## Compiler path

The smallest verified application path is:

```text
Python torch.nn.Module
        |
        | torch.export + Torch-MLIR FX importer
        v
Torch dialect -> Linalg-on-tensors -> bufferized scalar loops
        |
        | MLIR LLVM dialect -> LLVM IR -> Golem Clang/LLD
        v
freestanding RISC-V ELF -> one QEMU hart
```

The single-core proof is implemented in `tests/pytorch-single-core`. PyTorch
is a host-side compiler dependency only. The generated ELF contains the
lowered model, a small memref ABI driver, and the existing bare-metal startup
and UART code; it neither embeds Python nor requires an operating system.

## Build-time composition

Pinned upstream submodules remain unmodified. The build creates detached
worktrees and overlays project-owned integration code:

```text
third_party/qemu
  + components/devices/mittens-{sync,nic,analog}
  + components/qemu/golem-analog
  + bridge/include/mittens/{Sync,NIC,Analog}TileBridge.h
  + patches/qemu
      -> build/sources/qemu

third_party/sst-elements
  + components/elements/mittens
  + bridge/include/mittens/{Sync,NIC,Analog}TileBridge.h
      -> build/sources/sst-elements
```

The QEMU patches register the synchronization, NIC, and analog devices, attach
them to the RISC-V `virt` machine, connect fd 41 to precise TCG icount, and add
the five Golem instruction patterns and their translation helper. The NIC is
guest-visible at `0x10010000`; the synchronization and analog devices have no
guest MMIO range. The SST Elements preparation enables only Merlin and
Mittens.

Bare-metal tests are linked separately for each tile. This permits a tile ELF
to contain only its local computation and runtime state; routine code is not
transferred over the mesh.

## Tile startup and shutdown

For a managed tile with both interfaces enabled, `mittens.tile` performs this
sequence:

1. Configure the `SST::Interfaces::SimpleNetwork` subcomponent.
2. Create and map the fixed-size synchronization `memfd`.
3. Create and map the per-tile NIC `memfd`.
4. Create and map the geometry-sized analog `memfd`.
5. Stage the close-on-exec bridge descriptors and duplicate them to 41, 42,
   and 43 in the QEMU child without descriptor-remapping collisions.
6. Launch QEMU with precise icount and all enabled bridge properties.
7. Grant instruction quanta and schedule returned instruction counts on
   `cpu_clock`.
8. Process NIC, analog, and optional task-marker boundaries only after their
   fd 41 yield reaches its scheduled SST cycle.
9. Advance active analog arrays on `analog_link_clock`.
10. Schedule the guest's normal finisher write as an fd 41 `GUEST_EXIT`
    boundary, then reap QEMU at that simulated cycle.
11. Treat a nonzero or signal-based QEMU exit as a fatal simulation error.
12. Terminate and reap any surviving child during cleanup.

The effective QEMU command for an attached tile is:

```text
qemu-system-riscv64 \
  -machine virt -smp 1 -m 16M \
  -bios none -kernel <tile.elf> \
  -display none -monitor none -serial stdio -no-reboot \
  -icount shift=0,sleep=off \
  -global mittens-sync.bridge-fd=41 \
  -global mittens-nic.bridge-fd=42 \
  -global mittens-analog.bridge-fd=43
```

The child maps the bridge during device realization, validates its ABI header,
and closes its copy of the file descriptor after `mmap`. Both processes retain
their mappings until tile teardown.

## Packet data path

A guest transmission follows this sequence:

1. The tile waits for `STATUS.TX_READY`.
2. It writes the final endpoint ID to `TX_DESTINATION`.
3. It writes a 32-bit payload to `TX_DATA`, which is the transmit doorbell.
4. QEMU appends `{destination, payload}` to fd 42.
5. QEMU yields `NIC_TRANSMIT` through fd 41 when the 64-entry transmit ring
   fills. A partial ring is serviced at the next fd-41 synchronization
   boundary.
6. SST advances to that CPU boundary and `mittens.tile` drains the ring.
7. It creates one 32-bit SST network request for every bridge entry. Each
   request carries endpoint IDs and a `PacketEvent` containing one 32-bit
   payload.
8. Merlin selects router ports, applies buffering and link timing, and delivers
   the request to the destination endpoint.
9. The destination `mittens.tile` places `{source, payload}` in its fd 42
   receive ring when Merlin delivers the request.
10. If the destination hart is stopped on `RX_WAIT`, Mittens resumes it
    through fd 41 at that delivery time. QEMU's wait operation rechecks the
    receive rings, so a packet arriving between `RX_VALID` and `RX_WAIT`
    cannot be lost.
11. The destination QEMU exposes `STATUS.RX_VALID`; `RX_SOURCE` peeks the
    oldest entry's sender and `RX_DATA` consumes its 32-bit payload.

Source identity lets the deployment runtime keep one frame decoder per source.
Words from different source tiles may therefore interleave at a destination
without combining two tensor transfers.

Deployment frames take a bounded bulk path instead. The runtime submits the
five-word frame header and tensor payload in chunks of at most 4096 words.
QEMU snapshots each chunk into fd 42 and Mittens creates one multi-flit Merlin
request. Merlin carries ordered 32-bit words at the physical width configured
by the SST topology. At the destination, guest software consumes the
five-word frame header through `RX_SOURCE`/`RX_DATA`, validates it, and
registers the payload's local tensor address with the NIC. QEMU reports that
descriptor through fd 41. Mittens schedules delivered fd-42 payload bursts on
the tile's receive DMA channel and authorizes each burst only at its modeled
completion cycle. QEMU then copies the authorized burst into the guest range
and reports a source-and-route completion. The fallback transport still
consumes each payload through `RX_DATA`.

```text
source tensor RAM
       |
       v
QEMU TX snapshot --> fd 42 --> SST mesh --> fd 42 --> SST RX DMA timer
                                                        |
                                                 authorization at completion
                                                        |
                                                        v
                                              QEMU functional RAM write
```

All 32-bit words cross the same SST links and incur the same routing,
serialization, buffering, and contention. Receive DMA removes destination
guest MMIO instructions; it does not remove network traffic. One local DMA
channel serializes writes per tile, while different tiles' channels overlap.

## Analog accelerator path

The analog design adds a second, tile-local QEMU/SST path. It is independent
of the mesh NIC and its `NICTileBridge`. The complete path through one tile is:

```text
+---------------------------- ONE SIMULATED TILE -----------------------------+
|                                                                              |
|  Bare-metal RISC-V program                                                   |
|                                                                              |
|    mvm.set matrix, array_id      Program matrix                              |
|    mvm.l   vector, array_id      Load input vector                           |
|    mvm     array_id              Start matrix-vector multiplication           |
|    mvm.s   output, array_id      Wait for and store result                    |
|    mvm.mv  source, destination   Move result between local arrays             |
|                          |                                                   |
|                          v                                                   |
|  +---------------------- QEMU riscv64 virt -------------------------------+  |
|  |                                                                        |  |
|  |  Custom instruction decoder                                            |  |
|  |             |                                                          |  |
|  |             v                                                          |  |
|  |  Golem instruction helper                                              |  |
|  |    - decodes rs1, rs2, and the array ID                                |  |
|  |    - snapshots matrices and vectors from private guest RAM             |  |
|  |    - copies completed outputs back into private guest RAM              |  |
|  |             |                                                          |  |
|  |             v                                                          |  |
|  |  mittens-analog device                                                 |  |
|  |  mittens-sync device: grant / yield / resume on fd 41                  |  |
|  +-------------+----------------------------------------------------------+  |
|                |                                                             |
|                | shared-memory memfd                                         |
|                | QEMU file descriptor 43                                     |
|                v                                                             |
|  +------------------------- AnalogTileBridge -----------------------------+  |
|  | Header: tile ID, array geometry, queue geometry, and link width         |  |
|  |                                                                        |  |
|  |  array channel 0       array channel 1              array channel N     |  |
|  |  +---------------+     +---------------+          +---------------+     |  |
|  |  | four slots    |     | four slots    |          | four slots    |     |  |
|  |  |               |     |               |          |               |     |  |
|  |  | FREE          |     | FREE          |          | FREE          |     |  |
|  |  | SUBMITTED     |     | SUBMITTED     |          | SUBMITTED     |     |  |
|  |  | ACCEPTED      |     | ACCEPTED      |          | ACCEPTED      |     |  |
|  |  | COMPLETED     |     | COMPLETED     |          | COMPLETED     |     |  |
|  |  +-------+-------+     +-------+-------+          +-------+-------+     |  |
|  +----------|---------------------|--------------------------|-------------+  |
|             |                     |                          |                |
|             v                     v                          v                |
|  +--------------------------- SST mittens.tile ---------------------------+  |
|  |                                                                        |  |
|  |  SharedAnalogMemoryBridge                                              |  |
|  |             |                                                          |  |
|  |             v                                                          |  |
|  |  AnalogDevice                                                          |  |
|  |                                                                        |  |
|  |  ordered queue 0       ordered queue 1          ordered queue N         |  |
|  |  depth 4               depth 4                  depth 4                 |  |
|  |       |                     |                        |                  |  |
|  |       v                     v                        v                  |  |
|  |  256-bit link 0        256-bit link 1           256-bit link N          |  |
|  |  bidirectional         bidirectional            bidirectional           |  |
|  |       |                     |                        |                  |  |
|  |       v                     v                        v                  |  |
|  |  +---------+           +---------+              +---------+             |  |
|  |  | Array 0 |           | Array 1 |              | Array N |             |  |
|  |  | matrix  |           | matrix  |              | matrix  |             |  |
|  |  | input   |           | input   |              | input   |             |  |
|  |  | output  |           | output  |              | output  |             |  |
|  |  +----+----+           +----+----+              +----+----+             |  |
|  |       +---------------------+-------------------------+                  |  |
|  |                             |                                            |  |
|  |                             v                                            |  |
|  |                 selectable numerical backend                            |  |
|  |                             |                                            |  |
|  |                +------------+------------+                               |  |
|  |                |                         |                               |  |
|  |                v                         v                               |  |
|  |       Native C++ backend        CrossSim backend                         |  |
|  |       exact float32 MVM         modeled analog behavior                  |  |
|  +------------------------------------------------------------------------+  |
|                                                                              |
+------------------------------------------------------------------------------+
```

fd 43 carries only the analog command and bulk data shown vertically above.
After publishing a slot, QEMU separately yields `ANALOG_SUBMIT` through fd 41.
SST advances to the reported instruction boundary, reads fd 43, and later
resumes QEMU through fd 41. `StoreVector` remains stopped until the selected
slot is complete; fd 43 never wakes the hart by itself.

The complete functional path is implemented. A tile with nonzero
`analog_array_count` owns one `AnalogDevice`, a common analog-link clock, and
an independently allocated native C++ or CrossSim backend. One simulation-wide
array count, row count, and column count define the fixed analog geometry of
every tile.
The two dimensions need not be equal. Every array ID remains local to its
tile. Every CrossSim backend is independently owned by its tile and contains
one distinct CrossSim `AnalogCore` per array, ensuring that array state is
never shared between arrays or tiles.

Every analog array has its own tile-to-array channel and its own bidirectional
256-bit link:

```text
                        +-- array channel 0 -- 256-bit link <--> array 0
QEMU <-> Analog bridge +-- array channel 1 -- 256-bit link <--> array 1
                        +-- array channel 2 -- 256-bit link <--> array 2
                        `-- ...
```

The channels use the same configured clock frequency but have independent
bandwidth and transfer state. There is no tile-wide analog-link arbiter. If
`N` arrays are active, all `N` links may each advance one 256-bit beat in
either direction during the same simulated cycle, giving a maximum aggregate
link bandwidth of `N * 256` bits per cycle. One array link carries at most one
beat total per cycle; Platform v0.1 does not model simultaneous full-duplex
transfer on one array.

For example, independent links may overlap as follows:

```text
Analog-link cycle          0       1       2       3       4       5

Array 0 link             [SET]   [SET]   [LOAD]  [MVM]   [MVM]   [STORE]
Array 1 link             [SET]   [SET]   [LOAD]  [MVM]   [MVM]   [STORE]
Array 2 link             [LOAD]  [MVM]   [MVM]   [MVM]   [STORE] [IDLE]
                           ^       ^       ^       ^       ^       ^
                           Every active array may advance one independent
                           256-bit beat during the same SST cycle.
```

QEMU decodes each custom analog instruction and submits a command containing
only an operation and the values read from `rs1` and `rs2`:

```text
AnalogCommand = { operation, operand0, operand1 }
```

The initially agreed operations are:

| Operation | `operand0` (`rs1`) | `operand1` (`rs2`) |
| --- | --- | --- |
| `SetMatrix` | Matrix address | Local array ID |
| `LoadVector` | Input-vector address | Local array ID |
| `Compute` | Local array ID | Unused |
| `StoreVector` | Destination address | Local array ID |
| `MoveVector` | Source array ID | Destination array ID |

Commands are ordered within each array and may progress independently across
different arrays. QEMU snapshots guest memory for `SetMatrix` and
`LoadVector`, submits the command to the selected array channel, and may
retire the instruction once that command is accepted. `Compute` is likewise
asynchronous after acceptance. A full per-array queue applies backpressure
only to the instruction targeting that array. `StoreVector` is the natural
join: it waits only for its selected array's output before copying that output
to guest memory. `MoveVector` creates a source-to-destination array dependency
without serializing unrelated arrays.

Each QEMU tile still has one guest hart. To expose array concurrency, software
issues work to multiple arrays before reaching a blocking `StoreVector`. Once
the hart blocks on that join, it cannot issue later instructions until the
selected result is ready; work already submitted to every array continues in
SST.

Because QEMU owns private guest RAM, an SST `AnalogDevice` cannot dereference
a guest address directly. `AnalogTileBridge` therefore provides one
array-indexed command stream and local bulk-data path per array. QEMU copies
matrix and vector contents between guest memory and those paths; the SST side
meters their delivery over the corresponding array links. Eagerly snapshotting
input memory before an instruction retires ensures later guest writes cannot
change an already submitted operation.

Each array's bidirectional data path transfers at most one 256-bit beat per
analog-link cycle. One beat is 32 bytes, or eight 32-bit tensor elements. A
transfer containing `M` float32 elements on one array consequently requires
`ceil(M / 8)` link cycles in either direction. The final beat may contain
unused words. `StoreVector` returns `rows` float32 values and therefore takes
`ceil(rows / 8)` array-to-tile link cycles. Transfers for other arrays consume
their own links and do not increase that latency.

This 256-bit local path does not change the mesh word contract. Communication
between different tiles remains an ordered stream of 32-bit words. The SST
mesh width controls how many of those words a physical link can carry per
cycle, independently of each array's fixed 256-bit local link.

CrossSim determines the numerical result, including configured analog
nonidealities. Its host execution duration is not simulated hardware time.
SST owns each array's modeled transfer and accelerator latency and schedules
completion independently for every array. CrossSim calls may execute
sequentially on the host while SST still models their hardware intervals as
overlapping.

`AnalogTileBridge.h`, the QEMU device and decoder, the SST `AnalogDevice`,
native C++ and CrossSim MVM backends, per-tile configuration, bounded
per-array queues, bidirectional transfer timing, and fd 41 completion
handshakes are
implemented. The end-to-end proof compiles one bare-metal ELF containing all
five instructions, queues work on two arrays, reads both results, exercises
`MoveVector`, and passes against both numerical backends.

The ownership boundary can be summarized as:

```text
QEMU                                      SST
------------------------------------      ------------------------------------
Executes RISC-V instructions              Models accelerator time
Owns private guest RAM                    Owns array scheduling
Snapshots command inputs                  Advances independent 256-bit links
Blocks on mvm.s                           Applies configured compute latency
Copies returned output                    Runs the Native or CrossSim MVM
```

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

- Platform v0.1 uses an explicit `RX_WAIT` doorbell rather than NIC
  interrupts. A waiting hart resumes when delivered data becomes visible.
- The legacy NIC path carries one payload per transaction. The deployment path
  supports bounded 4096-word host transactions represented by 16 KiB Merlin
  timing cells. At the default 32-bit, 1 GHz mesh width, each cell is charged
  4096 link cycles; wider SST configurations reduce that serialization time.
- There is no operating system, dynamic loader, pthread runtime, or OpenMP
  runtime in a tile.
- The CPU timing policy is one retired instruction per cycle. Pipeline, cache,
  TLB, and memory-system timing are not yet modeled.
- UART output is functional and is not assigned detailed device latency.

These limits determine which timing measurements are meaningful. See
[`timing-model.md`](timing-model.md) before interpreting simulation times or
cycle counts.
