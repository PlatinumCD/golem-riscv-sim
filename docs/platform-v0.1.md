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

The bare-metal ELF is loaded at `0x80000000`. `platform/tile.ld` defines the
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
- 16 MiB of private RAM;
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
  -bios none -kernel <tile.elf> \
  -display none -monitor none -serial stdio -no-reboot \
  -icount shift=0,sleep=off \
  -global mittens-sync.bridge-fd=41 \
  -global mittens-nic.bridge-fd=42 \
  -global mittens-analog.bridge-fd=43
```

Descriptor 41 is the per-child instruction and event synchronization bridge.
Descriptor 42 is the tile's anonymous shared-memory NIC data bridge.
Descriptor 43 is the independently created analog command/data bridge. fd 41
is present for every managed tile; a tile receives fd 42 and fd 43 only for
interfaces enabled in its SST configuration.

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
number of instructions executed at the quantum end or an earlier device
boundary, and SST schedules that event after the same number of `cpu_clock`
cycles. Platform v0.1 initially defines one retired instruction as one CPU
cycle. `cpu_clock` defaults to 1 GHz and `sync_instruction_quantum` defaults
to 1,000 instructions.

Mesh transmissions and every analog submission yield through fd 41. fd 42 and
fd 43 carry only their device data. A normal SiFive finisher write yields
`GUEST_EXIT` through fd 41 before QEMU terminates, making the last retired
instruction boundary part of the SST schedule. See
[`timing-model.md`](timing-model.md) for the exact measurement boundary and
the limits of the initial functional CPU timing policy.

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
  transfer state, completion state, and bidirectional 256-bit link.

Each configured array has a tile-local, zero-based array ID and owns a distinct
matrix, input, and output state. The CrossSim backend creates one distinct
CrossSim `AnalogCore` per configured array, with no analog model state shared
between arrays or tiles.

The required tile-local topology is:

```text
                        +-- channel[0] -- 256-bit link <--> array 0
QEMU <-> Analog bridge +-- channel[1] -- 256-bit link <--> array 1
                        +-- channel[2] -- 256-bit link <--> array 2
                        `-- ...
```

All array links use `analog_link_clock`, but they are independent physical
lanes rather than clients of one shared lane. No tile-wide arbiter divides
their bandwidth. Each array link carries at most one beat in one direction
during a cycle. Platform v0.1 does not provide simultaneous tile-to-array and
array-to-tile transfer on the same array link.

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
because an unrelated array is busy.

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

Every analog array has its own bidirectional link of exactly 256 bits. Each
active array may advance one 32-byte beat in either direction during the same
analog-link cycle:

```text
one beat = 8 x 32-bit words = 256 bits = 32 bytes
link cycles for M float32 elements in either direction = ceil(M / 8)
maximum aggregate bandwidth with N active arrays = N * 256 bits per cycle
```

There is no bandwidth arbitration between array links. For example, three
active arrays may all transfer beat zero in cycle zero and beat one in cycle
one. The common `analog_link_clock` parameter is initially intended to match
the modeled tile clock. `analog_compute_latency_cycles` sets each array's
modeled compute delay for either backend. Arrays schedule their compute
completion independently, so their modeled compute intervals may overlap.

The operation transfer directions and lengths are:

| Operation | Link direction | Float32 words | Link cycles |
| --- | --- | ---: | ---: |
| `SetMatrix` | Tile to array | `rows * columns` | `ceil((rows * columns) / 8)` |
| `LoadVector` | Tile to array | `columns` | `ceil(columns / 8)` |
| `Compute` | None | `0` | `0` |
| `StoreVector` | Array to tile | `rows` | `ceil(rows / 8)` |
| `MoveVector` | Source array to tile, then tile to destination array | `rows` in each direction | `ceil(rows / 8)` beats on each participating link |

`MoveVector` requires `rows == columns`, as defined by the instruction
contract. Its source and destination links are distinct and may stream
simultaneously once a source beat is available; each link still transfers no
more than one beat in its active direction per cycle.

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
| `analog_link_clock` | Common clock frequency for every independent bidirectional array link | `1GHz` |
| `analog_compute_latency_cycles` | Compute delay in link cycles | `100` |

All tiles in one simulation must use the same `analog_array_count`,
`analog_array_rows`, and `analog_array_columns`. These three values define
fixed hardware geometry and cannot be changed by guest software at runtime.
For example, count `3`, rows `100`, and columns `64` mean that every tile owns
three arrays and every array stores one `100 x 64` matrix.
SST simulation configurations should define these values once in a global
parameter set and apply that set to every `mittens.tile`.

The SST `AnalogDevice` implements bounded per-array queues and independent
transfer, compute, and completion state. Every active array advances once per
analog-link clock tick. `SetMatrix`, `LoadVector`, `StoreVector`, and
`MoveVector` are metered at eight float32 words per link cycle, and compute
completion is delayed by `analog_compute_latency_cycles`. The native and
CrossSim backends use the same transport and timing state machine.

The analog path is local to a tile. It does not bypass or widen the mesh:
tile-to-tile communication still transfers exactly one 32-bit word at a time
through the mesh NIC and Merlin.

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

The mesh NIC is a polling, programmed-I/O device. Each packet carries one
32-bit destination tile ID and one 32-bit payload. The payload is normally the
raw IEEE-754 representation of a 32-bit float; the NIC transports its bits
without interpreting or converting them.

All NIC registers are little-endian, 32-bit registers and require aligned
32-bit accesses. Register offsets are relative to the NIC base address
`0x10010000`.

| Offset | Register | Access | Meaning |
| ---: | --- | :---: | --- |
| `0x00` | `STATUS` | RO | Transmit and receive readiness |
| `0x04` | `TX_DESTINATION` | WO | Destination tile ID |
| `0x08` | `TX_DATA` | WO | Write one payload to transmit a packet |
| `0x0c` | `RX_DATA` | RO | Read and consume one received payload |

`STATUS` has the following bit assignments:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `TX_READY` | The NIC can accept a write to `TX_DATA` |
| 1 | `RX_VALID` | `RX_DATA` contains a payload that can be consumed |
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

### Receive behavior

Software waits for `RX_VALID` and reads `RX_DATA`. The read returns the oldest
waiting 32-bit payload and consumes that packet. If another packet is queued,
it becomes visible through `RX_DATA` and `RX_VALID` remains set.

The sender is implicit in the SST endpoint that injected the packet and is not
exposed to tile software. Current proof programs contain their computation
code directly in each tile image.

### Flow control and errors

SST provides backpressure and does not drop packets. Packets from a given
sender are delivered in order. Writing `TX_DATA` while `TX_READY` is clear,
reading `RX_DATA` while `RX_VALID` is clear, or using an invalid destination is
a platform protocol error. The simulator must stop with a diagnostic when it
detects one of these errors.

The following sequence is required for transmission:

```text
wait for TX_READY
write TX_DESTINATION
fence iorw, iorw
write TX_DATA
```

The following sequence is required for reception:

```text
wait for RX_VALID
read RX_DATA
```

## Bare-metal runtime

The Platform v0.1 runtime design is specified below. Its foundational types,
fixed bookkeeping structures, and transport-neutral 32-bit word interface are
implemented in `runtime/` as the freestanding `libgolem-runtime.a`. The tile
scheduler and tensor arena remain to be implemented.

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

The tile-to-tile channel transfers exactly one 32-bit word at a time. One NIC
write produces one 32-bit SST request and one 4-byte Merlin flit. There is no
wider path and no multiword network transaction.

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
The router treats every word independently and uses deterministic X-then-Y
routing for its destination tile.

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
The four mesh NIC registers, their access behavior, and their status bits are
also locked for Platform v0.1. The remainder of the NIC's 4 KiB region is
reserved and reads as zero; writes to it are ignored.
