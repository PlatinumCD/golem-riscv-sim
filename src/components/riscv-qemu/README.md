# RISC-V QEMU component

`tilecomponents.RiscvQemu` is a standalone RV64/RVV CPU component. Its process
launcher, synchronization bridge, execution ledger and ELF reader were copied
from `src/sst`; they now live here under a separate C++ namespace. The CPU
connects through its `qemu_memory` StandardMem slot to the same banked scratchpad
used by the other `src` clients.

```text
RISC-V QEMU → RiscvQemu → StandardMem → SPM bus → Scratchpad → BankedBackend
                                                   ↑
                                      other StandardMem clients
```

## Build and validate

From the repository root:

```bash
python3 -B src/components/riscv-qemu/build_qemu.py
python3 -B src/build.py
python3 -B src/tests/riscv-qemu/run.py
python3 -B src/tests/vector-memory/run.py
python3 -B src/tests/vector-analog/run.py
```

The first command builds a separate QEMU executable at
`build/src/qemu/qemu-system-riscv64`. It uses the pinned QEMU revision and
existing `src` patches, with the synchronization device overlay in this
directory. It does not replace `install/qemu` or change the production build.
The SST component is included in `build/src/components/libtilecomponents.so`.

The regression compiles a bare-metal RISC-V guest and runs it with an independent
SST memory client. The client supplies inputs and verifies the CPU's scalar/RVV
results through StandardMem. The guest also checks initialized data, zeroed
BSS, stack accesses, fences and externally modified code after `fence.i`.
Cases vary request size, channel bandwidth, instruction grant size and issue
width. Logs, bank service traces and JSON reports go under
`tests/results/source-new-riscv/`.

## Compose a simulation

Use this in an SST Python configuration with `src` on Python's import
path and `build/src` plus the installed Elements library on `SST_LIB_PATH`:

```python
from configuration import connect_riscv

cpu, scratchpad = connect_riscv(
    sst,
    {"spm_banks": 8, "spm_channels": 2, "spm_request_bytes": 32},
    elf="/absolute/path/to/program.elf",
    memory_file="/absolute/path/to/run/scratchpad.bin",
    cpu_parameters={"instruction_budget": 256},
)
```

`memory_file` is a simulation output and is freshly zeroed by this helper.
`qemu=` can override the default executable path. `clients=[interface, ...]`
connects additional StandardMem clients to the same SPM. The helper returns the
CPU and scratchpad. `connect_arrays()` separately instantiates arrays with a
payload command port and no memory interface.

`connect_riscv_arrays()` returns `(cpu, arrays, scratchpad)` and connects the
CPU's `analog_commands` port. It supports `mvm.vset`, `mvm.vl`, and `mvm.vs`
register chunks plus `mvm` execution and scalar active-region configuration.
Configure uses CUSTOM_0 (`0x0b`), funct3=7, funct7=9: `rs1` is the array slot,
`rs2` packs `(uint64_t(rows) << 32) | cols`, and `rd` receives zero on success
or nonzero on rejection. It is available through `.insn r 0x0b, 7, 9, rd, rs1, rs2`;
no assembler mnemonic extension is needed. The instruction conservatively drains
prior scalar, vector-memory, and analog-transfer queues before issuing. An
unread array result still causes a nonzero status, avoiding a wait that would
prevent its own required Store. Slots wider than 32 bits are rejected before
conversion, rather than aliasing a valid slot.
See the [instruction contract](vector-analog.md)
for operands, LLVM compilation, chunk timing, and validation.
The array command link is their only external port. The arrays have no SPM
connection or fd43 analog device. Legacy `mvm.set/l/s/mv` instructions trap
before any memory access.

Link all ELF `PT_LOAD` segments, including code, data, BSS and stack, into
`[0x90000000, 0x90000000 + spm_capacity_bytes)`. Other SST clients address the
SPM from offset zero. The test's [linker script](../../tests/riscv-qemu/scratchpad.ld)
is a complete example. Boot initialization is untimed. The CPU and SPM run at
the existing fixed 1 GHz clock. SPM capacity defaults to 2 MiB; CPU composition
accepts explicitly configured capacities up to 32 MiB, divisible by the request
size and 4096 bytes. Standalone CPU composition uses one SST rank and one thread.
Complete meshes support multiple SST workers through `connect_riscv_mesh`,
which keeps every tile's local components together. See
[threading](../../../docs/threading.md) for launch options and restrictions.

CPU options use `cpu_parameters`; VLEN is shared with the architecture:

| Parameter | Default | Meaning |
|---|---:|---|
| `instruction_budget` | 256 | Instructions allowed per host synchronization grant |
| `issue_width` | 1 | Total instruction issue slots per cycle, 1–4; scalar, vector and custom instructions all count |
| `instruction_fetch_width` | 0 | Cached sequential instructions per block, 1–4; 0 follows issue width |
| `integer_issue_units` | 2 | Integer ALU issue ports |
| `memory_issue_units` | 1 | Shared scalar/RVV memory-instruction issue ports |
| `integer_latency_cycles` / `integer_initiation_interval` | 1 / 1 | Integer result latency / cycles between admissions per port |
| `floating_latency_cycles` / `floating_initiation_interval` | 3 / 1 | Scalar FP latency / admission interval |
| `vector_latency_cycles` / `vector_initiation_interval` | 1 / 1 | RVV arithmetic latency / admission interval |
| `multiply_latency_cycles` | 3 | Integer multiply result latency; one admission per cycle |
| `divide_latency_cycles` | 16 | Divide/sqrt latency and admission interval |
| `load_store_queue_depth` | 1 | Outstanding vector memory beats, 1–64; depth 1 preserves blocking behavior |
| `scalar_load_store_queue_depth` | 8 | Outstanding scalar operations, 0–64; zero selects blocking accesses |
| `analog_command_queue_depth` | 4 | Outstanding vector analog commands, 0–16; zero selects blocking transfers |
| `analog_command_queue_bytes` | 16384 | Active analog payload-byte limit, a multiple of four from 1024 through 16384 |
| `instruction_cache_enabled` | true | Enable the instruction cache; false uses direct SPM fetches |
| `instruction_cache_bytes` | 8192 | Instruction-cache capacity in bytes |
| `instruction_cache_line_bytes` | 64 | Bytes fetched from SPM on a cache-line fill |
| `instruction_cache_ways` | 2 | Ways per set; replacement uses least recently used tags |
| `instruction_cache_hit_cycles` | 1 | Latency to supply a cached fetch block |
| `riscv_vector_enabled` | true | Enable RVV 1.0 |
| `riscv_vector_length_bits` | 256 | Shared architecture VLEN, a power of two from 128 to 1024; also sets array link bytes/cycle to VLEN / 8 |
| `riscv_vector_element_bits` | 64 | RVV ELEN, 32 or 64 |
| `host_timeout_seconds` | 30 | Wall-time limit for one QEMU rendezvous |
| `serial_output` | empty | Absolute serial log path; empty uses stdout |

Set `riscv_vector_length_bits` in the architecture parameters to configure both
the CPU and arrays. Its existing `cpu_parameters` spelling also works, provided
the two locations do not conflict. Resolve both together with
`resolve(parameters, cpu_parameters=cpu_parameters)` when saving a full parameter
snapshot. Array link width is derived automatically; it cannot be tuned separately.

Cache capacity, line size, and associativity must be positive powers of two.
Capacity is at most 16 MiB and must hold at least one line per way. Lines are
at least four bytes, fit inside SPM, and divide its capacity. Hit latency is a
positive integer. Invalid settings fail before the helpers create components
or change the SPM backing file, including when the cache is disabled.

## Memory and timing contract

The scalar queue defaults to `scalar_load_store_queue_depth=8`; set it to zero
for blocking scalar accesses. See the [scalar queue contract](scalar-load-store-queue.md).
The separate default `load_store_queue_depth=1` retains blocking vector accesses.
Set it to 2–64 to overlap eligible independent vector loads/stores while
preserving timed data visibility and dependencies. See the
[load/store queue contract](load-store-queue.md) for eligibility, ordering,
traps, counters and synchronization details.

QEMU and the SST controller map **one shared SPM backing file**. There is no
second functional SPM or copy-back synchronization. The local QEMU device adds
`scratchpad-fd` and `scratchpad-strict-sync` properties. Strict mode stops before
each scalar access or safely grouped RVV memory beat. It disables the old
post-access timing batches, which would execute accesses before SST had modeled
their completion.

The [in-order issue scheduler](instruction-issue.md) enforces a total issue budget,
register readiness and execution-unit limits. Set `issue_width=2` for dual issue;
single issue remains the default. Arithmetic has separate result latency and
initiation interval, while memory and device completion retain their existing
queue timing. Synchronization budgets do not change the instruction schedule.

Instruction fetches check an 8 KiB, two-way instruction cache by default, with
64-byte lines and least recently used replacement. Hits use the configured
hit latency once per sequential fetch block; its width follows `issue_width`
unless `instruction_fetch_width` is explicit. Misses fetch complete lines through StandardMem, split at
`spm_request_bytes` boundaries; these fills consume actual SPM bank, port, and
channel service. An instruction spanning two cache lines can require two fills.
Setting `instruction_cache_enabled=false` sends each instruction fetch directly
to SPM instead.

Data loads and stores remain uncached. Ordinary unit-stride RVV loads/stores,
strided operations whose stride equals their element size, and whole-register
transfers combine contiguous active elements into beats of at most VLEN/8 bytes. Beats
stop at mask gaps and page boundaries; short VL, vstart and LMUL are respected.
Each beat is announced before its functional memory accesses. A nonfaulting
preflight must prove ordinary shared-SPM RAM with the required permissions.
Unsafe or unsupported cases retain the element path, preserving QEMU's normal
fault-prefix and restart behavior. Noncontiguous strided, indexed, segmented,
fault-only-first and element-misaligned accesses retain conservative element
timing. Naturally aligned elements may still cross SPM request boundaries;
those beats are split without losing their bank-level concurrency.

SST splits each beat at `spm_request_bytes` boundaries and submits its distinct
line fragments together. Bank ports, channels and contention determine service:
with eight four-byte banks an aligned 32-byte beat uses eight banks in one
service cycle; a 64-byte beat needs at least two service cycles. These are
service rates, not complete CPU request/response latencies. LMUL groups use
successive VLEN-sized beats rather than gaining a wider datapath.

QEMU resumes only after every request fragment completes. Its authorized
shared-memory accesses then run before the next SST event, with no extra
per-element timing charges. Exact byte ranges remain held until those accesses
commit. This models a blocking vector memory instruction with concurrent
fragments within a beat; separate beats and CPU instructions remain ordered.

A pre-store stop has no store payload. The generated scratchpad controller's
`external_write_requestor` parameter identifies exactly `riscv:qemu_memory`;
those writes consume normal bank service and return normal completion responses
without writing a placeholder payload into SPM. QEMU commits the real value
after completion. Other clients retain their normal functional read/write path.
This option requires mmap backing. For uncached data reads/writes and direct
instruction fetches with the cache disabled, the controller retains each access's
byte range after returning the timing response. A zero-delay `external_commit` link
releases it only after QEMU performs the access. This prevents an overlapping client
from reading stale bytes or changing the CPU's read value while its response
travels back through the bus.

Disjoint ranges can proceed independently even within a single transport fragment
boundary. Older overlapping requests retain priority, including requests from
other SPM clients. The request width remains `spm_request_bytes`.

Instruction-cache fills are actual SST reads. Their byte ranges are released
when all responses for that cache line arrive, before installing the tag or
starting the next line fill. There is no deferred QEMU access for a cache fill.
This also permits cache lines smaller than the SPM request size without
retaining a range across the next fill.

Instruction-cache fills complete before the corresponding instruction proceeds.
Vector accesses block at vector LSQ depth 1; scalar accesses block when
`scalar_load_store_queue_depth=0`. Enabled queues allow independent work to overlap. Analog register transfers block
when `analog_command_queue_depth=0`; depths 1–16 let independent CPU work
continue after guaranteed admission. Source registers remain pinned until timed
capture, and outputs remain pending until completion, including at LSQ depth 1.
See the [analog command queue contract](analog-command-queue.md). The default
`array_pipeline_enabled=True` resumes `mvm` after the array validates
and starts computation. Later completion uses independent token tracking,
allowing CPU input/output work while computation runs. Memory and instruction
fences, task markers, and guest exit drain all started computations. This does
not consume their buffered outputs. Array pipelining and RVV memory grouping
are independent; neither creates an array-SPM path. Set `array_pipeline_enabled=False`
explicitly to make `mvm` wait for completion. See the [pipeline contract](vector-analog.md#array-pipeline).
Unsupported synchronized device commands fail explicitly. Guest failures and
grants with no modeled instruction fetch also fail, so an unmapped trap loop
cannot silently consume synchronization grants forever. `fence.i` invalidates
the instruction-cache tags and flushes QEMU translations, so code written
through another SPM client is fetched again and can execute.
`RISCV_STATS` reports instruction counts, split request/completion counts,
logical `instruction_bytes`, actual SPM instruction traffic in `fetch_bytes`,
data bytes, and simulated cycles. Its `icache_` counters report logical fetches,
hits, misses, fills, fill bytes, evictions, invalidations, and added stall
cycles. A miss can fill one or two lines. Cache counters are zero when disabled.
`SPM_STATS` and the existing bank traces account for physical memory service.
`vector_memory_beats`, `vector_read_bytes` and `vector_write_bytes` count
preauthorized RVV beats separately from scalar/fallback accesses. The optional
`<cpu-name>-memory.csv` records each logical data access at issue and timing
completion, with its PC, address, byte count, direction and vector-beat flag.
The scratchpad trace records the actual fragments and per-bank service.

When `TILE_COMPONENT_OUTPUT` is set, `<cpu-name>-tasks.csv` records existing
guest task-start/task-finish markers with the SST cycle and cumulative CPU
instruction, issue-cycle, memory, instruction-cache, and analog counters. Logging adds no modeled
latency; the guest's marker instructions still consume normal execution and
fetch time. Subtract matched `(task_id, execution_id)` snapshots to measure
regions including SST stalls; QEMU's `rdcycle` does not measure those stalls.
The [single-tile benchmark](../../tests/single-tile-runtime/README.md) includes
the marker assembly and an empty-region overhead control. Task trace MMIO
uses the synchronization channel and does not require a NIC transport bridge.

For large single-tile experiments, optional
`TILE_COMPONENT_TRACE_START_TASK=<unsigned decimal task ID>` suppresses detailed
CPU, scratchpad and array observations until that task's first start marker.
Task snapshots and final counters are never filtered. The gate is shared by
the components in one SST process and is intended for one measured tile;
it does not define an independent trace window per CPU. A task marker drains
prior memory/array work, so no request is partly recorded across the gate.
The filter changes only logging, adds no modeled latency, and also enables the
array's [compact initial-program checksum](../analog-arrays/README.md#observations).
