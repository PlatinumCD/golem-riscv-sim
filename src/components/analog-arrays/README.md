# Analog arrays

One component contains `arrays_per_tile` independent arrays (one by default,
addressed as array 0). Each stores an
`array_rows × array_cols` row-major float32 matrix, an input vector and an output
vector. Functional arithmetic computes `y = W × x`, using a double accumulator
and float32 outputs. Timing comes from the configured operation costs and
register transfers; this does not simulate analog noise or device physics.

## Commands

[commands.h](commands.h) defines the command/completion event. Commands identify
an array, a live token, an operation, an element offset and an element count.
Program/Load payloads contain little-endian float32 bit patterns. Weight offsets
use flat row-major matrix order; input and output offsets index their vectors.

| Operation | Behavior |
|---|---|
| `Program` | Transfer exactly `elementCount × 4` payload bytes into weights starting at `elementOffset`, then wait according to `array_program_delay_scope` |
| `Load` | Transfer exactly `elementCount × 4` payload bytes into input elements starting at `elementOffset` |
| `Execute` | Wait `cost_per_mvm_cycles` and calculate the MVM; offset/count must be zero and the payload empty |
| `Store` | Transfer selected output elements into the successful `Complete` reply's payload; the request payload must be empty |

Arrays receive data only through register-transfer commands. The component has
one `commands` port and no memory interface. Ordinary CPU vector loads/stores
access the scratchpad separately; custom array instructions transfer register
contents over the array link.

With `array_pipeline_enabled=False`, each array executes accepted commands in order, while different arrays progress
independently. Four command slots per array include the active command. A full
queue returns `Busy` without accepting the command. Invalid arrays, operations,
ranges, payload sizes and duplicate live tokens return `Error`. Execute before
full initialization and nonempty Store before a computed result return `Error`
when the command reaches the front of the queue.

`Accepted` and `Complete` are separate responses. A token may be reused after
completion or error. Replies preserve the operation, array, token, element offset
and count. Only a successful Store completion contains payload bytes; other
replies do not echo input data. Rejected commands do not modify array state.

CPU requests may set `deferred=true` for Program, Load and Store. The backend
then sends `Guaranteed` instead of `Accepted` only after ruling out later
architectural errors. It projects initial-program coverage across admitted
chunks and reserves Store result identity/coverage. Uncertain cases retain
`Accepted` and blocking completion; a guaranteed command that later fails is
a simulator protocol error. Deferred Execute is unsupported and retains the
existing scalar Started/Complete contract. Queue capacity and scheduling do
not change.

Deferred Program/Load send `Captured` after their final timed input byte is
consumed, before any programming delay. Even an empty input command emits this
response. It releases the CPU source-register pin while the command may remain
outstanding. Store data is published only in Complete. See the CPU
[analog queue contract](../riscv-qemu/analog-command-queue.md).

Partial updates preserve untouched elements. Initialization coverage is tracked
for every weight and input element, and the first Execute requires both to be
fully initialized. Coverage persists after execution and later partial updates.
In blocking mode, nonempty Program and Load commands invalidate the previous result; Store may
return different slices of a computed output through multiple commands.

A zero-count transfer is an accepted no-op. Its offset may equal the corresponding
element capacity, and its payload must be empty. It uses no link bandwidth or
programming cycles and preserves readiness and result state. A zero-count Store
returns an empty payload without requiring a computed result.

## Execution pipeline

`array_pipeline_enabled` defaults to true. Set it explicitly to false for blocking
execution. When true, each array has one
compute engine, one register-transfer engine, and two result slots. Execute
validates initialization, snapshots the input, reserves an output slot, and
sends `Started` after `Accepted`. It sends `Complete` after the usual compute
latency. The CPU can continue after `Started`; validation errors are returned
before that response. Computations never overlap each other on one array.

Load can prepare the next input while compute runs. Store reads the oldest
ready result while another computation runs. Completed results remain intact
through subsequent input loads. Successful Store chunks accumulate unique-row
coverage; covering all rows releases the oldest slot. Repeated partial reads
and zero-length transfers do not prematurely release it. Execute waits when
both slots are occupied. Queued Store commands may bypass an Execute blocked
on compute or result capacity, allowing direct command clients to drain the
FIFO. Loads do not overtake queued Executes. CPU guests must drain before
submitting a third job because their submission instruction waits for start.

Nonempty Program is rejected while a started/queued execution or an undrained
result exists, protecting the resident weights. Four command slots still bound
accepted work, and transfers still obey the configured link and byte-buffer
limits. No memory interface is introduced. Completed undrained results are
valid storage at simulation finish; live commands are not.

## Transfer timing

The array group's aggregate bytes per cycle equal architectural VLEN divided
by eight: `array_link_width = riscv_vector_length_bits / 8`. VLEN is the number
of bits in one vector register and supports 128, 256, 512 or 1024 bits, with a
default of 256 bits (32 bytes/cycle). The CPU and arrays use the same VLEN.
There is no independent link-width knob. A manually supplied `array_link_width`
must equal the derived value; the component rejects mismatches even when its
parameters are supplied directly through SST.

`array_link_duplex="shared"` makes inputs and outputs share one bandwidth budget;
`"independent"` provides separate input and output budgets of that width.
Round-robin arbitration selects arrays. Link transit takes one fixed cycle.

A full LMUL=1 register needs one link-service cycle when the in-flight window
holds the register and the link is uncontended. A full integer-LMUL=N register group
needs at least N service cycles. Contention and smaller buffers can add cycles;
completion also includes link transit and the applicable operation cost.

`array_inflight_bytes` limits bytes per array in the link pipeline. Chunks use
the remaining transfer length and available in-flight space, including short
byte tails when the window is smaller than one float32 element. Space is released
after a chunk crosses the link and completes its transit. Queuing more commands
does not create additional link capacity. Array geometry and these link settings
are independent of scratchpad capacity, request sizes, banks and channels.

With the default `array_program_delay_scope="per_command"`, each nonempty Program
command pays `cost_per_array_program_cycles` exactly once,
after its last byte arrives. This cost applies to each submitted register chunk;
there is no implicit programming epoch or separate commit operation. The command
remains active until programming finishes. Each array has its own deadline.
Subsequent MVMs reuse initialized weights, and zero-count Program commands incur
no programming cost. Load and Store have no additional fixed operation cost.

The opt-in `array_program_delay_scope="initial_full_array"` uses a fresh array as
one initial programming epoch. Chunks receive no per-command programming delay.
After unique received-weight coverage first reaches the entire matrix, the
coverage-completing command waits `cost_per_array_program_cycles` exactly once.
It remains outstanding and the array is unavailable for execution until that
delay expires. Duplicate or overlapping writes before full coverage replace
their selected values without advancing coverage twice; reaching the last
address alone does not complete an array with missing weights.

Once full coverage arrives, further nonempty Program commands are rejected,
including commands already queued before coverage completed and new requests
during or after the delay. Zero-length commands remain no-ops, and Load,
Execute and Store can reuse the resident weights. The restriction applies even
when the delay is zero. For the initial programming pass, zero delay preserves
the default mode's timing. Both modes support the execution pipeline and give
each array its own deadline. Repeated whole-array programming epochs are not
supported by this mode. This is an abstract whole-array timing assumption,
not a physical NVM programming model or a new memory path.

Input/output registers and the vectors used to assemble functional payloads are
distinct from the modeled link pipeline; they do not create extra bandwidth.

## Observations

With `TILE_COMPONENT_OUTPUT` set, `arrays.csv` records command boundaries and link
service with `event,cycle,array,token,operation,bytes,buffered,element_offset,
element_count`. `bytes` is the service size on link rows; command boundary rows
use zero. `array-requests.csv` records chunk allocation/release with the same
metadata plus `chunk_offset`, a byte offset within that command's payload.
`array-waits.csv` records stalls caused by a full in-flight window.

The allocation event precedes the buffered-byte increment. Consumers should pair
allocation/release events when reconstructing occupancy. These traces are
observations only and do not alter modeled scheduling. `ARRAY_STATS` reports
accepted/completed commands, errors, queue rejections, MVMs, link bytes and peak
buffered bytes per array.

`array-programming.csv` separately records programming observations without
changing the events in `arrays.csv`. Its columns are
`event,cycle,array,token,scope,element_offset,element_count,initialized_weights,total_weights,delay_cycles`.
Each nonempty successful Program emits `delivered` when its final byte completes
link transit, one cycle after its last link-service event. `initialized_weights`
counts uniquely received elements, including that command, while execution
readiness still waits for programming. `delay_cycles` is the configured cost.
Charged commands emit `delay_start` at delivery and `delay_complete` exactly the
configured number of cycles later. The command's normal `complete` event occurs
at that same completion cycle. With zero cost both delay events still appear
at delivery. Initial-full-array mode emits one such pair per fully programmed
array; partial and duplicate chunks emit only `delivered`. No-op and rejected
commands emit no programming events.

`ARRAY_STATS` also reports `program_delay_scope`, `program_delay_charges`
(including zero-cycle charges), `program_delay_cycles` (sum of charged cycles),
and `initial_full_array_completions`. The final counter is zero in default mode.

Large single-tile studies can set `TILE_COMPONENT_TRACE_START_TASK=2` to
discard detailed setup observations until the CPU sees task 2's start marker.
Task snapshots and final statistics remain complete; all detailed traces from
that marker onward retain their normal format. The marker already drains prior
work, and this observation filter adds no scheduling events or simulated time.

That filter also enables `array-initial-program.csv`; alternatively, set
`TILE_COMPONENT_PROGRAM_PROOF=1` to retain this proof with full detailed traces.
In `initial_full_array` mode it records one row per committed initial epoch:
array ID, first command start, final commit cycle/token, scope, successful
nonempty program command count, delivered bytes, unique initialized/total
weights, configured delay, one charge/completion, and `weights_fnv1a64`.
The hash is unsigned decimal FNV-1a-64 (offset 14695981039346656037, prime
1099511628211, arithmetic modulo 2^64) over the actual resident matrix's
little-endian FP32 bytes in row-major order. It is a reproducibility checksum,
not a cryptographic proof. Total bytes plus unique coverage detect duplicate
or missing weight transfers; the checksum checks committed values. Fine
setup ordering and bank service can only be re-audited when their full traces
are retained. Neither optional environment variable changes functional work,
byte traffic, delays or final counters. Without them, tracing is unchanged.
