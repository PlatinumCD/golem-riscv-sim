# Vector load/store queue

Set `cpu_parameters={"load_store_queue_depth": 8}` when composing the CPU.
The supported range is 1 through 64. Depth 1 uses blocking vector accesses.
Scalar accesses use the separate [scalar queue](scalar-load-store-queue.md),
which defaults to eight entries and can be disabled with depth zero.
Depths greater than 1 enable a non-speculative,
in-order vector load/store unit with multiple outstanding requests. This is
independent of `array_pipeline_enabled`.

An entry represents one vector memory beat, at most `VLEN / 8` bytes. Its
fragments split at `spm_request_bytes` boundaries and compete for the ordinary
SPM banks, ports and channels. A 512-bit beat therefore uses one queue entry
and two 32-byte requests. Queue capacity is separate from the backend's request
queue; increasing it does not add bank ports or channel bandwidth. A whole-register
or integer-LMUL group uses up to eight such entries, one per active physical
register. A group larger than the queue is admitted incrementally as entries
retire; it remains one architectural instruction. The complete access is
preflighted before its first beat is submitted.

## Data and timing

QEMU produces the architectural instruction and operands. SST owns request
admission, bank service, responses and completion timing. One shared SPM backing
file remains the functional memory; no speculative shadow SPM is introduced.

1. The QEMU helper proves the entire eligible access is ordinary, nonfaulting,
   page-local SPM. It captures the address, width, destination metadata and,
   for stores, the actual vector-register bytes.
2. It publishes an immutable descriptor in a free shared slot and yields to
   SST. After admitting the entry and issuing its fragments, SST resumes QEMU
   without waiting for the memory response.
3. Independent instructions can execute. Pending loads reserve their physical
   vector destination until the returned bytes are applied. Reads and writes
   of that destination wait; stores use their captured data even if the source
   register subsequently changes.
4. SST receives every fragment response while the controller still holds the
   accessed byte ranges. It snapshots load values at this timed point.
   Entries retire in issue order. Stores update the shared SPM with their
   captured payload only at retirement; loads publish their captured payload.
5. The controller releases each retired entry's byte ranges. QEMU applies
   returned load bytes at a safe instruction boundary before any dependent
   instruction executes, and then makes the shared slot reusable.

A unique increasing token identifies every entry across slot reuse. Slot state
uses acquire/release publication; no QEMU pointers cross the process boundary.
Store visibility and load values cannot be moved earlier merely to obtain a
faster timing trace.

The current unit issues in program order and retires in program order; it does
not model speculative execution, register renaming, or store-to-load forwarding.
Overlapping byte ranges serialize, including partial overlaps and accesses by
other SPM clients. Disjoint ranges can overlap even inside one 32-byte transport
fragment boundary. A younger request cannot bypass an older overlapping waiter.
For example, accesses to bytes 0–15 and 16–31 can both reach the bank scheduler;
an access to bytes 8–23 waits for either active range it overlaps. Each external
commit names the exact address and size of every completed request fragment.
The controller validates the whole commit before releasing any range.

## Dependencies and precise boundaries

The existing per-instruction fetch rendezvous carries a mask of queue slots
which must finish before that instruction. SST stalls that instruction until
the mask is satisfied, while unrelated memory service continues. Queue-full
waits likewise yield to SST until a slot can be reclaimed. Neither side spins
waiting for simulated time to advance.

Scalar integer loop instructions and explicitly decoded vector moves, `vadd`,
`vid`, `vslideup`/`vslidedown`, and `vredsum` can proceed when independent of
pending vector loads. Dependencies cover every physical register in an integer
LMUL group; scalar/vector moves and reduction scalar operands cover one register.
Legal `vsetvl`, `vsetvli`, and `vsetivli` can proceed because older transfers
retain their original register, element-width, VL and tail metadata. Unknown
instructions drain the queue. Fences, instruction fences, task boundaries,
guest exit and scalar memory fallbacks drain outstanding requests. Eligible
scalar loads/stores use their own queue and may overlap vector traffic. Trap entry also drains older work, including instruction-fetch faults
which happen before the runtime fetch rendezvous.

Independent scalar arithmetic includes the base compressed integer ALU operations
and legal RV64 M/Zmmul multiply/divide/remainder operations. They retain ordinary
instruction issue timing and QEMU execution, but do not drain vector queues just
because they are excluded from fetch-segment batching. This does not introduce a
divider latency model or change the scalar execution cost. Disabled extensions,
reserved encodings, memory accesses, CSRs and fences retain their existing checks.

Legal `mvm.vset` and `mvm.vl` wait for pending loads into their source register
group; `mvm.vs` waits for pending loads into its destination group. The mask
covers the complete physical LMUL group, including preserved tails and
fractional-register aliases. Scalar `mvm` has no vector-register dependency.
Captured stores no longer depend on their original source registers. Unrelated
LSQ entries may therefore finish during an analog transfer. QEMU checks these
dependencies before accessing registers, and SST retains the same mask when
dispatching the command. Invalid encodings and unsafe contexts keep the
conservative drain; dynamic analog errors still pass through the ordinary
trap-entry drain before the handler observes architectural state.

With `analog_command_queue_depth=0`, the three analog register-transfer helpers
block until completion. Enabling the separate [analog command queue](analog-command-queue.md)
adds source pins and pending output groups to the same dependency checks. A
vector load cannot overwrite an analog source before capture, and loads, stores
and ALU operations cannot access a pending analog output. Captured ordinary
SPM stores keep their own data and do not pin their source registers. This
joint tracking also runs with blocking memory at LSQ depth 1.

For eligible loads, the destination, active element count and tail policy are
captured at issue. Completion preserves the original QEMU element endianness
and tail semantics. A younger instruction cannot overwrite an unfinished
load destination. For a short-VL grouped load with agnostic tails, the final
active beat reserves and fills all remaining tail-only registers at completion;
those tails do not create memory requests. No speculative exception rollback is required because only
fully preflighted accesses are admitted asynchronously.

## Supported asynchronous accesses

The asynchronous path covers:

- Unmasked, unit-stride, single-field loads/stores with integer LMUL 1/2/4/8
  and EEW=SEW. Short VL preserves QEMU's tail policy.
- Whole-register loads `vl1re{8,16,32,64}.v` through `vl8re{8,16,32,64}.v`,
  with groups of 1/2/4/8 registers, and stores `vs1r.v`/`vs2r.v`/`vs4r.v`/`vs8r.v`.
  These ignore VL and vtype, including `vill`, as required by their semantics.

Both require `vstart=0`, legal group alignment, aligned elements and the entire
active access within one ordinary SPM page. A group crossing a page falls back
before any asynchronous beat is submitted. VLEN remains 128 through 1024 bits.

Admission also requires the protected machine-mode execution context used by
the synchronization bridge: RV64 M-mode, interrupts masked, no active PMP rules,
security configuration, instruction-count triggers, breakpoints or stepping.
Stores to translated-code pages remain synchronous so QEMU performs code
invalidation correctly. Unsupported forms drain outstanding work and use the
existing architectural path, including masks, segments, strided/indexed forms,
fractional LMUL, EEW/SEW mismatch, restart state and fault-prefix handling.
They remain correct but do not gain asynchronous memory overlap.

This is a bounded vector-LSU model, not a complete out-of-order CPU. Extending
eligibility requires corresponding dependency and exception handling; accepting
an instruction because it resembles an existing encoding is insufficient.

## Choosing a depth

Queue depth is a hardware capacity in physical-vector beats, not a count of
architectural instructions. At VLEN 512, four entries hold at most 256 bytes;
eight hold 512 bytes and admit one full LMUL=8 transfer without waiting for a
slot. A smaller queue is supported and refills as individual entries retire.
Completion does not wait for the entire vector instruction or a host grant.

Use the amount of independent work and the timed memory path to choose capacity.
For a stream of full beats, `depth * (VLEN / 8)` is its maximum outstanding
payload. Covering `target_bytes_per_cycle * response_latency_cycles` is a useful
first estimate, but is not a throughput guarantee: register dependencies,
ordering boundaries, partial transfers and competing clients can limit progress.
An LMUL=8 instruction followed immediately by a consumer of that group cannot
use arbitrarily deep queues to hide its own dependency.

Measure both elapsed time and bank-service gaps when increasing depth. A smaller
queue-full counter can simply move waiting to a register dependency or drain.
Keep one entry per beat and the existing completion rules; freeing an entry
before its response or silently increasing depth would hide real resource costs.

## Synchronization cost and observations

Completion checks and load-register updates reuse instruction fetch rendezvous.
There is no new per-cycle IPC loop, and SST response events do not wake a running
QEMU process independently. A full queue or a trap/fallback drain publishes an
explicit wait event so the single SST event thread can continue servicing
memory. Host rendezvous time is separate from simulated cycle accounting.

`riscv-lsq.csv` records increasing tokens, slot numbers, address/size/direction,
occupancy, and enqueue, issue, service-complete and retirement cycles. Enqueue
occupancy includes the new entry; complete occupancy excludes the retired entry.
Stall records distinguish register dependencies, a full queue and drains.
`riscv-memory.csv` retains its existing columns; at depth greater than 1 its
issue/ready records can interleave. Use the tokenized LSQ trace for unambiguous
matching of asynchronous accesses.

`riscv-waits.csv` records completed LSQ waits with exact start/end cycles,
dependency reason, synchronization stop reason and guest PC. It distinguishes
in-kernel drains from the final task-boundary drain without inferring an end
from unrelated asynchronous completions. Logging adds no simulated events or
issue delay. The PC identifies a waiting instruction fetch when available;
queue-full protocol waits can report PC zero. Instruction-cache lookup PCs can be used for dynamic instruction
classification in successful runs, provided their phase counts reconcile with
the retired task counters.

CPU statistics expose `lsq_enqueued`, `lsq_completed`, `lsq_peak_occupancy`,
`lsq_register_stalls`, `lsq_full_stalls`, `lsq_drain_stalls` and `lsq_stall_cycles`.
Task counters include queue submissions, completions and blocked cycles.
Markers drain the queue, so reported phase duration includes final delivery,
not merely request submission. Summed overlapping request latencies may exceed
elapsed phase time and must not be interpreted as CPU stall time.

Validation lives in [`src/tests/load-store-queue/`](../../tests/load-store-queue/README.md).
The [analog dependency suite](../../tests/analog-register-dependencies/README.md)
checks true dependencies, independent overlap, captured stores, LMUL/tail
preservation, precise traps, and instruction-budget-independent timing.
The focused [LLVM RVV check](../../tests/llvm-rvv/README.md) reuses saved compiler
binaries to validate asynchronous admission without rerunning the application sweep.
The [SPM/RVV transfer study](../../../studies_new/transfer-bandwidth/SPM-to-RVV/README.md)
sweeps queue depth independently of VLEN and bank count.
