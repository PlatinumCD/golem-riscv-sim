# Golem Tile Platform v0.1

This document defines the software-visible address map for version 0 of a
single bare-metal Golem tile. Each tile has one RISC-V hart and its own private
address space.

## Address map

| Start | End | Size | Device or region | v0 use |
| ---: | ---: | ---: | --- | --- |
| `0x00100000` | `0x00100fff` | 4 KiB | QEMU test/exit device | Simulation exit |
| `0x02000000` | `0x0200ffff` | 64 KiB | RISC-V timer (CLINT) | Reserved; unused initially |
| `0x0c000000` | `0x0fffffff` | 64 MiB | RISC-V interrupt controller (PLIC) | Reserved; unused initially |
| `0x10000000` | `0x10000fff` | 4 KiB | UART | Debug and console output |
| `0x10010000` | `0x10010fff` | 4 KiB | SST mesh NIC | Mesh communication |
| `0x80000000` | `0x80ffffff` | 16 MiB | Tile-private RAM | Bare-metal ELF, static data, heap, and stack |

The bare-metal ELF is loaded at `0x80000000`. `src/platform/tile.ld` defines the
text, read-only data, initialized data, and zero-initialized data regions. The
heap begins after the aligned BSS, and a 64 KiB stack occupies the top of tile
RAM. A linker assertion rejects images whose static data and heap boundary
overlap the reserved stack.

Addresses not listed above are not part of the Golem Tile Platform v0.1 software
ABI. QEMU may use other regions internally, but tile software must not depend
on them.

## Simulation architecture

Platform v0.1 uses one QEMU system-emulation process for each tile and one SST
`mittens.tile` component to own that process. QEMU executes the tile's RISC-V
instructions and services its local devices. SST owns the discrete-event
schedule, mesh topology, packet routing, and modeled network latency. QEMU
instruction progress is synchronized to that SST schedule through precise
icount grants on the per-tile fd 41 control bridge.

Each QEMU tile has:

- one RISC-V hart;
- standard RVV 1.0 execution enabled by default, with configurable `VLEN` and
  `ELEN`;
- 16 MiB of private RAM;
- an optional private SST memHierarchy data L1;
- one bare-metal ELF image loaded at `0x80000000`;
- the UART and simulation-exit devices listed in the address map; and
- one mesh NIC at `0x10010000`; and
- when enabled by SST, one tile-local analog device reached through the
  custom Golem instructions.

The QEMU processes do not share guest RAM. Data moves between tiles only as
packets through their mesh NICs.

### Tile lifecycle

SST is the lifecycle authority for every tile. During setup, `mittens.tile`
starts its QEMU child with the configured ELF and prevents SST from ending
while that child is running. The component monitors and reaps the child. A
normal QEMU exit releases SST's end-of-simulation hold; a signal or nonzero
exit is a simulation error. During normal or emergency SST shutdown, the
component terminates and reaps any QEMU child that is still running.

The initial implementation launches QEMU approximately as follows:

```text
qemu-system-riscv64 \
  -machine virt -smp 1 -m 16M \
  -cpu rv64,v=true,vext_spec=v1.0,vlen=256,elen=64 \
  -bios none -kernel <tile.elf> \
  -display none -monitor none -serial stdio -no-reboot \
  -icount shift=0,sleep=off \
  -global mittens-sync.bridge-fd=41 \
  -global mittens-sync.memory-timing=<on|off> \
  -global mittens-nic.bridge-fd=42 \
  -global mittens-analog.bridge-fd=43
```

Descriptor 41 is the per-child instruction and event synchronization bridge.
Descriptor 42 is the tile's anonymous shared-memory NIC data bridge.
Descriptor 43 is the independently created analog command/data bridge. fd 41
is present for every managed tile; a tile receives fd 42 and fd 43 only for
interfaces enabled in its SST configuration.

The conforming vector geometry is `VLEN=256` and `ELEN=64`. The corresponding
Mittens parameters are `riscv_vector_enabled`,
`riscv_vector_length_bits`, and `riscv_vector_element_bits`. QEMU supplies
RVV instruction semantics, while `src/platform/crt0.S` enables vector state in
`mstatus.VS`. Compiler vector-code generation is a separate target-feature
choice; the focused RVV test uses
`-march=rv64gcv_xgolemanalog` explicitly. Other accepted vector parameter
values describe experimental, non-v0.1 processor configurations. The
normative vector contract is defined in
[`vector-architecture.md`](vector-architecture.md).

The SST component has a linear `tile_id` used to identify its endpoint in the
mesh. Platform v0.1 does not yet define how software reads its own tile ID; a
future platform revision or a reserved NIC register may expose it if needed.

### Mesh data path

The intended packet path is:

```text
bare-metal tile program
        | mesh-NIC MMIO
QEMU mittens-nic device
        | shared-memory SPSC queues
SST mittens.tile
        | SST::Interfaces::SimpleNetwork
SST Merlin mesh
```

The QEMU NIC device converts guest MMIO operations into bridge operations.
Each tile owns one versioned host shared-memory region containing bounded
transmit and receive SPSC queues. The QEMU process produces the transmit queue
and consumes the receive queue; `mittens.tile` performs the opposite roles and
exchanges packets through `SST::Interfaces::SimpleNetwork`. This transport is
an implementation detail and does not change the four-register guest ABI
below.

Managed QEMU launch, monitoring, and cleanup; the QEMU NIC device; the
shared-memory bridge; and the SST SimpleNetwork/Merlin connection are
implemented and tested. The initial proof sends the float32 bit pattern for
`3.25` from tile 0 to tile 1 and returns an acknowledgment through the same
path. A second proof routes the same payload from `(0,0)` to `(2,2)` across a
3x3 mesh and verifies both four-hop directions with Merlin router statistics.
A third proof executes a nine-stage float32 computation pipeline in which
every tile adds its tile ID and forwards the result to a physical neighbor.

### Timing model

SST grants QEMU bounded precise-icount quanta through fd 41. QEMU reports the
total and vector instruction counts at the quantum end or an earlier device
boundary. SST schedules that event after:

```text
max(ceil(total instructions / cpu_issue_width), vector instructions)
```

`cpu_issue_width` accepts 1, 2, or 4 and defaults to 1, preserving the
original one-retired-instruction-per-cycle behavior by default.
`cpu_clock` defaults to 1 GHz and `sync_instruction_quantum` defaults to 1,000
instructions.

Every accepted deployment-burst descriptor, a full legacy scalar transmit
ring, a blocking `TX_WAIT`, and every analog submission yield through fd 41.
fd 42 and fd 43 carry only their device data. The deployment descriptor event
is a zero-wait timestamp: SST resumes the hart immediately unless its bounded
transmit ring remains full. A normal SiFive finisher write yields `GUEST_EXIT`
through fd 41 before QEMU terminates, making the last retired instruction
boundary part of the SST schedule. See
[`timing-model.md`](timing-model.md) for the exact measurement boundary and
the limits of the initial functional CPU timing policy.

### Optional tile-private data L1

`mittens.tile` supports `memory_backend=native|memhierarchy`. Native is the
default and leaves QEMU's private-RAM data path unchanged. The memHierarchy
backend requires a `memHierarchy.standardInterface` in the tile's `memoryIF`
slot. Each tile connects that interface to its own L1 instance; no L1 object
or cache state is shared between tiles.

When enabled, QEMU forces guest RAM data loads and stores through its TCG slow
path and publishes a `MEMORY_ACCESS` fd 41 event containing the physical
address, byte size, and read/write direction. SST sends the equivalent
StandardMem request and holds the hart until the response. QEMU then performs
the actual functional RAM access. Instruction fetches and MMIO do not use this
path.

This separation is intentional:

```text
QEMU private RAM = architectural bytes
SST private L1   = hit/miss state and completion timing
```

The initial proof uses a 32 KiB, four-way L1 with 64-byte lines and a simple
50 ns lower-memory backend. The topology is configurable in the SST Python
graph, not fixed in the guest ABI. The current implementation is blocking
with one outstanding CPU memory request per tile. It does not yet model an
instruction cache, TLB, scratchpad, nonblocking misses, or coherence with the
NIC receive-DMA path.

Deployments may enable a distinct initialization phase. Before the guest calls
`mesh_nic::complete_memory_initialization()`, QEMU counts RAM traffic locally.
That call emits one fd 41 `MEMORY_INIT_COMPLETE` handshake containing the
aggregate access, read-byte, and write-byte counts. SST charges a configurable
fixed latency plus `ceil(total_bytes / bytes_per_cycle)`, resumes the hart, and
then restores detailed per-access StandardMem timing. Platform v0.1 therefore
retains modeled initialization volume without requiring one host handshake for
every weight-loading access.

### Tile-local analog accelerator

Platform v0.1 implements a tile-local analog accelerator from compiler-emitted
instructions through QEMU and SST to a selectable native C++ or CrossSim
float32 MVM backend. `AnalogTileBridge.h` fixes the shared-memory command,
payload, status, geometry, and queue ABI. The exact LLVM instruction encodings
and operand lowering are recorded in [`analog-isa.md`](analog-isa.md). The
complete per-tile data-path diagram is in the
[architecture document](architecture.md#analog-accelerator-path).

An analog-enabled tile has:

- one tile-local analog control interface;
- one `AnalogDevice`;
- one independently owned numerical backend; and
- a configurable number of analog arrays, each with its own command stream,
  array state, and completion state.

All array payload transfers share one tile-wide bidirectional 256-bit link.

Each configured array has a tile-local, zero-based array ID and owns a distinct
matrix, input, and output state. The CrossSim backend creates one distinct
CrossSim `AnalogCore` per configured array, with no analog model state shared
between arrays or tiles.

The required tile-local topology is:

```text
                        +-- ordered channel[0] --+
QEMU <-> Analog bridge +-- ordered channel[1] --+--> round-robin arbiter
                        +-- ordered channel[2] --+             |
                        `-- ...                               |
                                                  shared 256-bit link
                                                            |
                                               +------------+------------+
                                               |            |            |
                                            array 0       array 1      array 2
```

The link uses `analog_link_clock` and is a single physical lane shared by all
array channels. A tile-wide round-robin arbiter grants at most one 256-bit
beat per cycle. The link is half-duplex: a cycle carries either one
tile-to-array beat or one array-to-tile beat, never both.

The custom instructions submit the following logical command:

```c
enum AnalogOperation {
    SetMatrix   = 1,
    LoadVector  = 2,
    Compute     = 3,
    StoreVector = 4,
    MoveVector  = 5,
};

struct AnalogCommand {
    AnalogOperation operation;
    uint64_t operand0;
    uint64_t operand1;
};
```

The operand interpretation is:

| Operation | `operand0` | `operand1` |
| --- | --- | --- |
| `SetMatrix` | Tile-memory matrix address | Array ID |
| `LoadVector` | Tile-memory input address | Array ID |
| `Compute` | Array ID | Unused |
| `StoreVector` | Tile-memory destination address | Array ID |
| `MoveVector` | Source array ID | Destination array ID |

The command does not carry a tile ID because the bridge belongs to exactly one
tile. The array ID selects an ordered command stream. Different array streams
may execute concurrently, while commands targeting the same array remain in
program issue order. Platform v0.1 does not require a command ID because it does
not permit independent reordering within one array stream.

The instruction issue and synchronization rules are:

| Operation | QEMU may retire when | Array-local ordering |
| --- | --- | --- |
| `SetMatrix` | The matrix has been snapshotted from guest memory and the command has been accepted | Later operations on that array wait for programming |
| `LoadVector` | The vector has been snapshotted from guest memory and the command has been accepted | Later compute on that array waits for loading |
| `Compute` | The command has been accepted | Starts after earlier commands on that array |
| `StoreVector` | That array's output is ready and has been copied to guest memory | Acts as the array-local join |
| `MoveVector` | The source-to-destination dependency has been accepted | Waits for the source output and precedes later destination operations |

A command instruction blocks only when its selected array's bounded queue
cannot accept it; `MoveVector` must be accepted by both participating array
streams. `StoreVector` additionally blocks when its selected array's result is
unfinished. An instruction targeting an available array is not delayed merely
because an unrelated array is computing, although its payload transfer may
wait for the shared link.

Because each tile has one guest hart, software exposes overlap by issuing work
to multiple arrays before joining:

```text
LoadVector(array 0)
Compute(array 0)       # accepted asynchronously
LoadVector(array 1)
Compute(array 1)       # overlaps array 0
StoreVector(array 0)   # first possible blocking join
StoreVector(array 1)
```

When the hart reaches a blocking `StoreVector`, it cannot issue subsequent
instructions until that selected result is ready. Commands already submitted
to other arrays continue to transfer and compute in SST.

QEMU owns the tile's private RAM. Guest addresses in analog commands are
therefore not host pointers and cannot be dereferenced by SST. QEMU snapshots
`SetMatrix` and `LoadVector` source words into the selected bridge slot before
publishing it. For `StoreVector`, SST publishes the result in that slot and
QEMU copies it to guest memory before the instruction retires.

Each array owns four shared-memory slots. QEMU publishes a monotonically
numbered slot in fd 43 and reports that slot through fd 41. SST copies the
payload into the array-local modeled queue, marks the slot accepted, and later
publishes its completion status and any output words. SST resumes a QEMU hart
through fd 41 after the required acceptance, queue-space, or blocking-store
boundary. fd 43 never independently resumes execution.

Every tile has exactly one bidirectional analog link of 256 bits, regardless
of its array count. Exactly one active array may advance one 32-byte beat in
one direction during an analog-link cycle:

```text
one beat = 8 x 32-bit words = 256 bits = 32 bytes
link cycles for M float32 elements in either direction = ceil(M / 8)
maximum aggregate bandwidth with N active arrays = 256 bits per cycle
```

Round-robin arbitration occurs at beat granularity. For example, three active
arrays receive successive grants in cycles zero, one, and two, then the
sequence repeats. The `analog_link_clock` parameter is initially intended to
match the modeled tile clock. `analog_compute_latency_cycles` sets each
array's modeled compute delay for either backend. Arrays schedule compute
completion independently, so their modeled compute intervals may overlap
each other and the shared-link transfer selected for that cycle.

The operation transfer directions and lengths are:

| Operation | Link direction | Float32 words | Link cycles |
| --- | --- | ---: | ---: |
| `SetMatrix` | Tile to array | `rows * columns` | `ceil((rows * columns) / 8)` |
| `LoadVector` | Tile to array | `columns` | `ceil(columns / 8)` |
| `Compute` | None | `0` | `0` |
| `StoreVector` | Array to tile | `rows` | `ceil(rows / 8)` |
| `MoveVector` | Source array to tile, then tile to destination array | `rows` in each direction | `2 * ceil(rows / 8)` shared-link cycles when uncontended |

`MoveVector` requires `rows == columns`, as defined by the instruction
contract. It performs the source-to-tile transfer first and the
tile-to-destination transfer second because both directions use the same
half-duplex link.

CrossSim computes the configured nonideal numerical result while SST continues
to determine modeled data-transfer and accelerator latency. CrossSim host
execution time is not used as simulated latency.

The implemented tile parameters are:

| Parameter | Meaning | Default |
| --- | --- | --- |
| `analog_array_count` | Simulation-wide array count on every tile | `0` |
| `analog_array_rows` | Simulation-wide row count for every array | `100` |
| `analog_array_columns` | Simulation-wide column count for every array | `100` |
| `analog_backend` | Numerical backend: `native` or `crosssim` | `native` |
| `crosssim_config` | Optional CrossSim JSON parameter path or built-in configuration name | Empty/default CrossSim parameters |
| `analog_link_clock` | Clock frequency for the tile-wide shared bidirectional 256-bit link | `1GHz` |
| `analog_compute_latency_cycles` | Compute delay in link cycles | `100` |

All tiles in one simulation must use the same `analog_array_count`,
`analog_array_rows`, and `analog_array_columns`. These three values define
fixed hardware geometry and cannot be changed by guest software at runtime.
For example, count `3`, rows `100`, and columns `64` mean that every tile owns
three arrays and every array stores one `100 x 64` matrix.
SST simulation configurations should define these values once in a global
parameter set and apply that set to every `mittens.tile`.

The SST `AnalogDevice` implements bounded per-array queues and independent
compute and completion state. A round-robin arbiter selects at most one array
transfer beat per analog-link clock tick. `SetMatrix`, `LoadVector`,
`StoreVector`, and `MoveVector` are metered at eight float32 words per granted
link cycle, and compute completion is delayed by
`analog_compute_latency_cycles`. The native and CrossSim backends use the
same transport and timing state machine.

The analog path is local to a tile. It does not bypass the mesh. Tile-to-tile
data remains a sequence of 32-bit architectural words through the mesh NIC
and Merlin; the SST topology separately configures how many such words a
physical link can transfer per cycle.

### Mesh coordinates and routing

Tiles use zero-based, row-major endpoint identifiers. For a mesh with width
`W`, the relationship between endpoint ID and coordinates is:

```text
tile_id = y * W + x
x = tile_id % W
y = tile_id / W
```

This conversion is network-internal. Guest software addresses a final tile ID
and does not calculate or carry coordinates in its 32-bit payload. Each Merlin
router derives and stores its own coordinates during construction. When a
packet enters the mesh, Merlin derives the destination coordinates once and
carries them in its internal routing event.

Platform v0.1 uses deterministic dimension-order routing. A router first moves
the packet along X until the destination X coordinate matches, then along Y,
and finally ejects the packet through the local port. In two dimensions, the
logical router port order is positive X, negative X, positive Y, negative Y,
then local. Edge ports in directions outside the configured mesh are left
unconnected.

## SST mesh NIC

The mesh NIC is a programmed-I/O device with an event-driven receive wait.
Its legacy operation carries one 32-bit payload. Its deployment operation
snapshots a bounded host burst of up to 4096 words and presents it to Merlin
as a multiword request. The corrected deployment baseline serializes that
request as one 32-bit timing cell per word; the host batching boundary is not
a physical flit width. Payloads are normally raw IEEE-754 representations of
32-bit floats; the NIC transports their bits without interpreting or
converting them.

All NIC registers are little-endian, 32-bit registers and require aligned
32-bit accesses. Register offsets are relative to the NIC base address
`0x10010000`.

| Offset | Register | Access | Meaning |
| ---: | --- | :---: | --- |
| `0x00` | `STATUS` | RO | Transmit and receive readiness |
| `0x04` | `TX_DESTINATION` | WO | Destination tile ID |
| `0x08` | `TX_DATA` | WO | Write one payload to transmit a packet |
| `0x0c` | `RX_DATA` | RO | Read and consume one received payload |
| `0x10` | `RX_SOURCE` | RO | Read the next payload's source without consuming it |
| `0x14` | `TX_BURST_ADDRESS_LOW` | WO | Guest physical source address bits 31:0 |
| `0x18` | `TX_BURST_ADDRESS_HIGH` | WO | Guest physical source address bits 63:32 |
| `0x1c` | `TX_BURST_WORD_COUNT` | WO | Burst length from 1 through 4096 words |
| `0x20` | `TX_BURST_SUBMIT` | WO | Snapshot and submit the described burst |
| `0x24` | `RX_WAIT` | WO | Yield until a receive word or burst is available |
| `0x28` | `TRACE_TASK_ID` | WO | Diagnostic global task ID |
| `0x2c` | `TRACE_EXECUTION_ID_LOW` | WO | Diagnostic execution ID bits 31:0 |
| `0x30` | `TRACE_EXECUTION_ID_HIGH` | WO | Diagnostic execution ID bits 63:32 |
| `0x34` | `TRACE_EVENT` | WO | Emit task start (`1`) or task finish (`2`) |
| `0x38` | `RX_DMA_SOURCE` | WO | Source tile for a receive-DMA descriptor |
| `0x3c` | `RX_DMA_ROUTE_ID` | WO | Route identity returned on completion |
| `0x40` | `RX_DMA_ADDRESS_LOW` | WO | Guest physical destination address bits 31:0 |
| `0x44` | `RX_DMA_ADDRESS_HIGH` | WO | Guest physical destination address bits 63:32 |
| `0x48` | `RX_DMA_WORD_COUNT` | WO | Exact number of 32-bit payload words |
| `0x4c` | `RX_DMA_SUBMIT` | WO | Register the described receive operation |
| `0x50` | `RX_DMA_STATUS` | RO | Receive-DMA submission and completion state |
| `0x54` | `RX_DMA_COMPLETION_SOURCE` | RO | Source tile of the oldest completion |
| `0x58` | `RX_DMA_COMPLETION_ROUTE_ID` | RO | Route ID of the oldest completion |
| `0x5c` | `RX_DMA_COMPLETION_ACK` | WO | Consume the oldest completion |
| `0x60` | `TX_WAIT` | WO | Yield until a deployment burst slot is available |

`STATUS` has the following bit assignments:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `TX_READY` | The NIC can accept a write to `TX_DATA` |
| 1 | `RX_VALID` | `RX_DATA` contains a payload that can be consumed |
| 2 | `TX_BURST_READY` | The NIC can accept a burst submission |
| 3-31 | Reserved | Read as zero |

`RX_DMA_STATUS` has the following bit assignments:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `SUBMIT_READY` | The NIC can accept another receive descriptor |
| 1 | `COMPLETION_VALID` | Completion source and route ID can be read |
| 2-31 | Reserved | Read as zero |

### Transmit behavior

Software waits for `TX_READY`, writes `TX_DESTINATION`, executes a RISC-V I/O
fence, and writes the raw 32-bit payload to `TX_DATA`. The `TX_DATA` write is
the transmit doorbell: it atomically forms and submits this packet:

```text
{ destination: TX_DESTINATION, payload: TX_DATA }
```

`TX_DESTINATION` remains latched until software changes it, allowing a tile
with a fixed successor to write its destination once during initialization.

For a bulk transfer, software waits for `TX_BURST_READY`, writes
`TX_DESTINATION`, the 64-bit guest physical source address, and a word count,
executes `fence rw, iorw`, and writes `TX_BURST_SUBMIT`. QEMU snapshots the
specified words before the command completes. The snapshot cannot change if
the guest subsequently reuses its source buffer. Every accepted burst then
publishes a zero-wait `NIC_TRANSMIT` descriptor doorbell on fd 41. SST sees
the descriptor at its exact simulated CPU timestamp and resumes QEMU
immediately when another slot is available.

If `TX_BURST_READY` is clear, software may perform independent work that does
not overwrite any unsent route buffer. When no such work is safe, it writes
`TX_WAIT`. QEMU rechecks the burst ring during that write; if it is still
full, QEMU yields `NIC_TRANSMIT_WAIT` and SST holds the hart until a burst slot
is free. Direct, non-managed QEMU returns from `TX_WAIT` and software
continues polling.

### Receive behavior

Software checks `RX_VALID`. When it is clear, software executes an I/O fence
and writes `RX_WAIT`. QEMU rechecks both receive queues in the same MMIO
operation. If data is already available, the write returns immediately. If
both queues remain empty under SST-managed execution, QEMU yields
`NIC_RECEIVE_WAIT` over fd 41. SST holds that tile stopped until a delivered
word or burst has been placed in its fd 42 receive bridge, then resumes QEMU
at that delivery time.

This check-and-yield sequence prevents a lost wakeup if a packet arrives
between the guest's `RX_VALID` read and its `RX_WAIT` write. Direct,
non-managed QEMU has no fd 41 authority, so `RX_WAIT` returns and the helper
continues polling.

Once `RX_VALID` is set, software reads `RX_SOURCE`, executes an I/O fence, and
then reads `RX_DATA`. `RX_SOURCE` peeks without consuming; `RX_DATA` returns
the oldest waiting 32-bit payload and consumes the `{source, payload}` entry.
If another packet is queued, it becomes visible and `RX_VALID` remains set.

### Receive-DMA behavior

Deployment software still reads the five-word frame header through
`RX_SOURCE` and `RX_DATA`. After validating the route, execution ID, and word
count, it registers the source tile, route ID, local tensor address, and exact
payload length through the receive-DMA registers. Bare-metal Platform v0.1
uses an identity-mapped address space, so the tensor pointer is also its guest
physical address.

QEMU associates each descriptor with one source tile and reports its
`{source, route_id, word_count}` to SST through an exact fd-41
`NIC_RX_DMA_SUBMIT` boundary. As fd-42 bursts from that source reach the head
of the receive bridge, `mittens.tile` schedules them on the tile-local receive
DMA engine. The payload remains hidden from the guest until that engine
completes. SST then authorizes the head burst, resumes a waiting hart, and
QEMU copies its ordered 32-bit words into the registered guest range.

A descriptor may span multiple bounded 4096-word host bursts. Setup is
charged only for its first burst. When exactly `RX_DMA_WORD_COUNT` words have
been authorized and copied, QEMU places `{source, route_id}` in the
completion queue. Software reads both completion fields and writes
`RX_DMA_COMPLETION_ACK`; only then does the runtime mark the route and its
local tensor resource ready.

Up to 64 descriptors or unacknowledged completions may be outstanding in
total. Only one descriptor may be active for a given source, while
descriptors for different sources can progress independently. A descriptor
must have a nonzero word count and a four-byte-aligned, non-overflowing guest
address. A received burst larger than the descriptor's remaining size is a
protocol error.

Receive DMA does not bypass SST or widen the architectural channel:

```text
source RAM
    -> transmit snapshot
    -> fd 42
    -> SST routers and links (every 32-bit word timed)
    -> destination fd-42 receive burst
    -> SST tile-local receive DMA (timed)
    -> SST authorization
    -> QEMU functional RAM write
    -> destination tensor RAM
```

Every tile owns one receive DMA channel. Transfers on the same tile serialize;
channels on different tiles advance independently. The default engine is
256 bits wide at 1 GHz with eight setup cycles per descriptor:

```text
descriptor service cycles =
    rx_dma_setup_cycles
    + sum(ceil(burst_word_count * 32 / rx_dma_width_bits))
```

The engine parameters are `rx_dma_clock`, `rx_dma_width_bits`,
`rx_dma_setup_cycles`, and `rx_dma_queue_depth`. The width must be a positive
multiple of 32 bits. Queue depth is from one through four and limits delivered
fd-42 bursts waiting for local service, so a full destination applies normal
SST network backpressure.

This models the NIC-to-local-memory write port. When the optional private L1
is enabled, CPU loads and stores are timed through a separate StandardMem
path; CPU traffic does not yet contend with this DMA port and DMA writes do
not update or invalidate L1 state. Capacity is still the QEMU private-RAM
allocation, and the final host memory copy is functional after the modeled
transfer completes. The legacy per-word receive interface remains available
and is the runtime fallback when receive-DMA callbacks are absent.

### Task trace behavior

Task tracing is an optional diagnostic path and does not add fields to mesh
packets. Software writes `TRACE_TASK_ID`, both halves of the 64-bit
`TRACE_EXECUTION_ID`, executes an I/O fence, and writes `TRACE_EVENT`. Under
SST-managed execution, QEMU emits an exact fd-41 `TASK_START` or
`TASK_FINISH` marker and waits for SST to resume it. SST first charges the
instructions retired before the marker, records its global simulated
timestamp, and resumes the hart immediately.

The deployment runtime can emit these markers immediately before and after
each registered task call. The compiler's global task ID and the runtime's
execution ID provide the trace identity. Trace markers are neither routed nor
visible to another tile:

```text
runtime task boundary
    -> NIC diagnostic MMIO
    -> QEMU fd 41 marker
    -> SST global timestamp
    -> immediate resume
```

The synchronized bridge carries `task_id` and `execution_id` in its fixed
control-plane record. Platform v0.1 bridge version 5 also carries the optional
memory address, size, and direction while preserving the 128-byte bridge
size.

### Flow control and errors

SST provides backpressure and does not drop packets. Packets from a given
sender are delivered in order. Writing `TX_DATA` while `TX_READY` is clear,
reading `RX_DATA` while `RX_VALID` is clear, or using an invalid destination is
a platform protocol error. An unsupported `TRACE_EVENT` value is also an
error. The simulator must stop with a diagnostic when it detects one of these
errors.

The following sequence is required for transmission:

```text
wait for TX_READY
write TX_DESTINATION
fence iorw, iorw
write TX_DATA
```

The following sequence is required for reception:

```text
read RX_VALID
if clear:
    fence rw, iorw
    write RX_WAIT
    retry
read RX_SOURCE
fence iorw, iorw
read RX_DATA
```

## Bare-metal runtime

The Platform v0.1 runtime source is in `third_party/sculptor-mlir/runtime/`.
`build-scripts/build-runtime.sh` cross-compiles this source as the
freestanding `libgolem-runtime.a` archive. The runtime consumes the compiler's
resource, dimension, workspace, route, and task-binding tables. It constructs
the concrete memref descriptors that generated task adapters require.

The first executable milestone also implements `BasicTileRuntime`. It consumes
the C ABI tables emitted by `sculptor-emit-golem-tile-abi`, validates their
core ownership and record layouts, runs every zero-input boot task once,
dispatches a compiled task by its global ID, and moves one generated route's
exact payload with blocking 32-bit NIC transfers. The two-linear proof uses
execution 0 and application-supplied ranked-memref descriptors for its fixed
`1x4`, `1x3`, and `1x2` tensors.

`DeploymentRuntime` adds the compiler-sized workspace, task readiness
scheduling, rank-generic contiguous descriptors, and source-aware framed
routes. Its frame format is:

```text
32-bit magic
32-bit route_id
32-bit execution_id low
32-bit execution_id high
32-bit payload word count
N x 32-bit payload words
```

Framed transmission is resumable. Each runtime step first consumes an
available receive word, then advances at most one pending header or payload
chunk. If the transmit path is full, the route ID, phase, and word offset
remain live for the next step instead of blocking inside the send call. This
is required for bounded networks: two tiles may exchange large tensors in
opposite directions without deadlocking when both transmit and receive queues
fill simultaneously.

If no local task or transmission can advance and at least one incoming route
is still outstanding, `DeploymentRuntime::step()` returns `WaitForReceive`.
The tile entry point then writes the NIC's `RX_WAIT` doorbell. Transmit
backpressure returns `Idle`, not `WaitForReceive`, because sleeping for an
incoming packet while an outgoing queue is the actual blocker could deadlock.

The initial implementation admits only execution ID 0. Multiple-execution
admission and reclamation remain later work.

The basic runtime is validated with both a two-layer 2x1 deployment and a
four-layer 2x2 deployment. The latter uses four compiler-generated tile ELFs,
one private analog array per tile, a boot-ready barrier, and three generated
activation routes along the snake path `0 -> 1 -> 3 -> 2`.

The runtime is a concurrent dataflow system. Multiple graph inputs move
through the task graph simultaneously. The runtime must keep the tensors,
waiting state, outputs, and completion state belonging to each graph input
separate so data from different inputs can never be combined accidentally.

### Execution ID

An execution is one complete input, or input set, being processed through the
entire neural-network task graph. The dispatch tile assigns that graph input
an `execution_id` before admitting it to the dataflow system. The ID remains
unchanged for the lifetime of that graph input.

```cpp
using ExecutionId = uint64_t;
```

The dispatch tile owns the execution-ID counter. It initializes the counter to
zero. For each newly admitted network input, it assigns the current counter
value as that input's `execution_id` and then increments the counter.

The compiled ELF declares `max_in_flight_executions`, matching the number of
simultaneous executions supported by its planned tensor buffers. The dispatch
tile never admits more active executions than this limit. The initial Platform
v0 configuration sets the limit to two. When an execution completes, its slot
becomes available for the next network input.

For example, if a second input is admitted while the first is still being
processed, two graph executions overlap:

```text
execution_id 0: first network input moving through the graph
execution_id 1: second network input moving through the graph
```

Every source tensor and every intermediate tensor derived from the first input
belongs to execution 0. The corresponding data for the second input belongs to
execution 1. If both executions reach the same task concurrently, the runtime
maintains two separate sets of waiting inputs:

```text
(execution_id 0, task_id 4): inputs belonging to the first graph input
(execution_id 1, task_id 4): inputs belonging to the second graph input
```

An `execution_id` does not identify a task, tile, or individual transferred
word. It identifies the complete graph input to which local task instances
belong. The current distributed proof admits only execution 0 and therefore
does not transmit an execution ID. How a future concurrent word stream
associates words with multiple in-flight executions is not yet defined.
Counter wraparound behavior is also not yet defined.

### Dataflow scheduling

A tile is never locked to one execution ID. Because one tile may host several
tasks, different tasks on that tile may simultaneously hold data belonging to
different executions.

When a tensor arrives, the runtime associates it with the pair
`(execution_id, task_id)`. It keeps the tensor with the other inputs belonging
to that same pair. The pair is not ready until all `Task::input_count` inputs
have arrived. Once complete, it becomes eligible to execute on the tile's
single core.

The tile has one ready queue shared by all of its registered tasks. The core
executes one ready item at a time, but an incomplete older execution does not
lock the tile or prevent a complete newer execution from running. The older
execution remains waiting for its missing data. Ready instances use the fixed
FIFO described below. Admission is limited by the compiled two-execution
buffer plan, and permanent stalls use the global-quiescence rule below.

### Task instance

One `TaskInstance` represents one registered task processing one graph
execution:

```cpp
struct Tensor;

enum class TaskInstanceState : uint32_t {
    Free = 0,
    WaitingForInputs = 1,
    Ready = 2,
    Running = 3,
    ForwardingOutputs = 4,
    Complete = 5,
    Failed = 6,
};

struct TaskInstance {
    ExecutionId execution_id;
    uint32_t task_id;
    Tensor* inputs;
    uint32_t received_input_count;
    Tensor* outputs;
    TaskInstanceState state;
};
```

The pair `(execution_id, task_id)` uniquely identifies the instance on a tile.
`inputs` points to a runtime-owned array whose length is the registered task's
`input_count`. Array index `i` stores input slot `i`. A missing slot has
`ElementType::Invalid`. `received_input_count` increments only when a
previously missing slot is filled. The instance becomes ready when
`received_input_count == Task::input_count`. Delivery to an already occupied
slot is a protocol error. `outputs` points to a runtime-owned array whose
length is `Task::output_count`. The runtime initializes it before `execute`
and retains it until required output forwarding is complete. Additional
execution state beyond `state` is not yet defined.

The state machine is:

```text
WaitingForInputs
    -> Ready                 when all inputs arrive

Ready
    -> Running               when selected by the tile

Running
    -> ForwardingOutputs     when execute returns Success
    -> Failed                when execute returns Failure

ForwardingOutputs
    -> Complete              when every output has been forwarded

Complete or Failed
    -> Free                  after runtime cleanup
```

Each tile allocates a fixed-capacity pool of `TaskInstance` objects during
initialization. The runtime does not grow this pool or allocate additional
instances from a heap while the tile is running. A completed or failed
instance is reset and returned to the pool for reuse.

```cpp
constexpr uint32_t kTaskInstanceCapacity = 16;
```

Each tile can therefore track at most 16 active `(execution_id, task_id)` pairs.
Tensor payload buffers are separate and are not included in this capacity.
The runtime finds an active pair by scanning these 16 entries and comparing
both identity fields. This lookup requires no secondary index or dynamic
memory. It is an internal implementation choice and may be replaced later
without changing the Platform v0.1 runtime ABI.

When the first tensor for an unseen `(execution_id, task_id)` pair arrives,
the runtime claims the first `Free` pool slot. It initializes the instance's
identity and input array and changes its state to `WaitingForInputs`. If that
arrival fills the task's final required input slot, the instance immediately
continues to `Ready`.

Each tile has one fixed 16-entry FIFO ready queue shared by every registered
task. Queue entries are task-instance pool indices, not pointers or copied
instances. When an instance enters `Ready`, its pool index is appended. The
single core removes and executes indices in readiness order. Because the queue
capacity equals the instance-pool capacity, it can hold every active instance.

```cpp
struct ReadyQueue {
    uint32_t slots[kTaskInstanceCapacity];
    uint32_t next_read;
    uint32_t next_write;
};
```

`next_write` counts entries appended and selects the next physical write slot
with `next_write % kTaskInstanceCapacity`. `next_read` counts entries removed
and selects the next physical read slot in the same way. The queue is empty
when both counters are equal and full when their unsigned difference equals
`kTaskInstanceCapacity`.

### Task

A task has an integer ID and an `execute` field that refers to the function
implementing that task:

```cpp
struct Tensor;

enum class TaskStatus : uint32_t {
    Success = 0,
    Failure = 1,
};

using TaskExecute = TaskStatus (*)(
    const Tensor* inputs,
    uint32_t input_count,
    Tensor* outputs,
    uint32_t output_count
);

struct Task {
    uint32_t id;
    TaskExecute execute;
    uint32_t input_count;
    uint32_t output_count;
};
```

`id` is globally unique across the complete deployed task graph. It does not
depend on which tile stores or executes the task.
The ID and `execute` function are assigned before registration. The runtime
does not generate IDs, renumber tasks, or select their implementation.
Tasks are registered during tile initialization. After initialization, that
tile's registered task set and every registered `Task` are read-only for the
remainder of the run.

The two pointers refer to contiguous arrays of small `Tensor` wrappers; tensor
data remains in separate runtime-owned buffers and is not copied into these
arrays. `TaskExecute` returns `TaskStatus::Success` after successful execution
and `TaskStatus::Failure` after any task error.

`input_count` is fixed when the task is registered. It does not change between
executions of the task and tells the runtime how many inputs must be ready
before it calls `execute`.
Input slots are zero-based and range from `0` through `input_count - 1`. In the
current fixed stream, the compiled receive schedule determines which local
task and input slot receives each word.

`output_count` is also fixed when the task is registered and does not change
between executions.

Each output's routing is fixed by the compiled tile ELF. The sender writes the
destination tile to `TX_DESTINATION`; the destination ELF's receive schedule
selects the local task and input slot. The current fixed stream does not
transmit task IDs or input-slot IDs. Task functions do not select destinations
dynamically.

### Tensor transfer

Every tile-to-tile tensor is represented as ordered 32-bit words. The legacy
NIC operation creates a one-word Merlin request. The deployment runtime groups
up to 4096 contiguous words into one bounded request. The architectural word
size is fixed, but the SST topology accepts a physical
`mesh_link_width_bits` parameter that must be a positive multiple of 32.

At `mesh_link_clock`, a width of `W` bits can transfer `W / 32` words per
cycle:

```text
link bandwidth = W * mesh_link_clock
timing-cell cycles = ceil((network_cell_words * 32) / W)
```

Timing cells are padded to a complete physical beat. The corrected default is
one 32-bit word per timing cell on a 32-bit, 1 GHz, 4 GB/s link, so each word
costs one cycle and a 512-word request costs 512 serialization cycles. A
64-bit link transfers two such words per cycle. Router capacity is configured
independently as 16,384 one-word cells, preserving the previous 64 KiB
capacity. These settings do not change NIC registers, frame fields, tensor
element representation, or QEMU/runtime ABIs.

Platform v0.1 does not add a runtime header. No task ID, execution ID, input
index, or payload length is transmitted before the data. The compiled tile ELF
already fixes the destination tile, receiving task order, input shape, element
type, and number of words expected.

For example, the distributed matvec proof passes a four-element `float32`
vector using four independent transfers:

```text
word 0: vector[0] float32 bits
word 1: vector[1] float32 bits
word 2: vector[2] float32 bits
word 3: vector[3] float32 bits
```

The receiver stores the first word in input slot 0, the second in slot 1, and
so on. After the fourth word arrives, that local input buffer becomes ready.
Legacy transfers remain independent packets. Deployment bursts share one
route and arbitration decision but consume one link beat per word using
deterministic X-then-Y routing.

This fixed stream deliberately supports one execution moving through the
current demonstration. Multiplexing multiple executions or dynamically
selecting among multiple destination tasks is outside this transfer rule and
must not be inferred from it.

#### Buffer banks and backpressure

The general runtime design reserves two buffer banks for every task input and
output, matching `max_in_flight_executions`. The current fixed-stream proof
uses only bank 0 for execution 0. Selecting a bank for multiple in-flight
executions depends on the future multiplexing rule and is not implemented by
this proof. An occupied bank remains unavailable until its tensor is no longer
required for execution or forwarding.

The word queues and mesh remain lossless and apply backpressure. A sender that
cannot enqueue the next 32-bit word pauses and retains its output data. Its
core may execute other ready instances. Once downstream space becomes
available, word transmission resumes. Backpressure may propagate through the
graph and temporarily stop the dispatch tile from admitting another
execution. No word or tensor may be dropped or overwrite an occupied buffer.

#### Completion and failure

The compiled graph identifies its terminal outputs. The dispatch tile tracks
every admitted execution ID, and an execution completes only after all of its
declared terminal outputs arrive.

Platform v0.1 is fail-fast. A task failure, invalid tensor transfer, duplicate
input, or invalid task ID stops the run with a diagnostic containing the
execution ID, task ID, and error. Platform v0.1 does not retry a failed task or
recover a failed execution.

Platform v0.1 can use SST simulated time for future stalled-execution policies
because QEMU instruction progress is synchronized through fd 41. The current
runtime design instead detects global quiescence: one or more executions remain active,
the mesh and transfer queues are empty, and no tile has a ready, running, or
forwardable task instance. This condition is a deadlock. The simulator stops
and reports every waiting `(execution_id, task_id)` pair and its missing input
slots. Temporary waiting is valid while any system progress remains possible.

### Task registry

Each tile has one immutable registry containing the tasks assigned to that
tile:

```cpp
struct TaskRegistry {
    const Task* tasks;
    uint32_t task_count;

    const Task* find(uint32_t task_id) const noexcept;
    bool valid() const noexcept;
};
```

`tasks` points to a read-only array of `task_count` preassigned `Task` entries.
The registry does not assign IDs or select task implementations. `find`
returns the matching task or `nullptr` when that task is not registered on the
tile.
The array is sorted by `Task::id`, and `find` uses binary search. Lookup does
not allocate memory or modify the registry. `valid` checks for a non-null task
array, non-null execute functions, and strictly increasing task IDs.

### Tensor

`Tensor` is the common type for every task input. Tensor rank means the number
of axes, not matrix rank from linear algebra:

```cpp
enum class ElementType : uint32_t {
    Invalid = 0,
    Float32 = 1,
    Int8 = 2,
    UInt8 = 3,
    Int16 = 4,
    UInt16 = 5,
    Int32 = 6,
    UInt32 = 7,
};

struct Tensor {
    ElementType element_type;
    int64_t rank;
    void* descriptor;
};
```

The runtime tensor representation must interoperate with the MLIR `memref`
ABI because task data arrives in that form. `descriptor` points to the exact
rank-specific MLIR memref descriptor. That descriptor contains the allocation
pointer, aligned data pointer, element offset, dimension sizes, and strides.
Those fields are not duplicated in `Tensor`.

| Rank | Shape |
| ---: | --- |
| 0 | Scalar |
| 1 | Vector |
| 2 | Matrix |

Tensor elements are typed. Platform v0.1 supports this initial element set:

- `float32`;
- `int8` and `uint8`;
- `int16` and `uint16`; and
- `int32` and `uint32`.

`element_type` tells the runtime how to interpret the tensor's raw data bytes.
Tensor rank, element type, sizes, strides, and storage requirements are fixed
by the compiled tile ELF. The runtime does not infer tensor shapes, partition
tensors, or dynamically plan tensor layouts. A task consumes and produces the
complete logical tensors assigned to it by the compiled graph.

Tensor elements cross the mesh as independent 32-bit words. The destination
ELF knows the fixed number of elements and fills its local buffer in the
compiled order. The task sees only the completed local tensor.

The runtime owns both the memref descriptor and its local data allocation. A
task borrows its input tensors for the duration of `execute` and must never
release their descriptors or data. After the task and any required forwarding
are complete, the runtime returns the storage to its reusable buffer pool.
Borrowed input tensors are read-only: `execute` must not modify either their
descriptor metadata or their underlying data.

Before calling `execute`, the runtime binds every output to a compiler-planned
memref descriptor and local buffer slot. The task borrows these outputs as
writable tensors and fills their data. The runtime retains ownership of the
output descriptors and buffers.

### Tensor data arena

Each tile reserves one fixed-capacity region of its private RAM as a shared,
reusable tensor data arena. The compiled ELF supplies the arena layout and
fixed buffer-slot requirements for that tile's tensors. The runtime does not
perform general variable-size allocation or tensor partitioning.

When a task instance needs tensor storage, the runtime claims an available
compiler-planned slot of the required layout. Incoming bytes are written into
that slot, and its MLIR memref descriptor points to the slot's local address.
Once the tensor is no longer needed for execution or forwarding, the runtime
marks the slot reusable. Tensor pointers are local to the tile and are never
transmitted over the mesh.

## Stability

The base addresses and region sizes in this table are locked for Platform v0.1.
The mesh NIC registers through offset `0x60`, their access behavior, and their
status bits are locked for Platform v0.1. The remainder of the NIC's 4 KiB
region is reserved and reads as zero; writes to it are ignored.
