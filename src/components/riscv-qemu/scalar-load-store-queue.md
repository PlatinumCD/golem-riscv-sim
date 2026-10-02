# Scalar load/store queue

`scalar_load_store_queue_depth` defaults to **8**. Set it to **0** to use
blocking scalar accesses, or to an integer from 1 through 64 to select the
maximum outstanding scalar operations. Small, Medium, and Large inherit this
default. It is independent of the vector `load_store_queue_depth` and the
analog command queue.

An entry represents one integer or floating-point load/store of 1, 2, 4, or
8 bytes. Compressed instructions use the same mechanism. The queue shares the
ordinary StandardMem interface, byte-range ordering, SPM banks, ports, and
channels with existing traffic. Its capacity does not create bank bandwidth.

## Admission, completion, and dependencies

QEMU computes each address and proves that the access is an aligned,
page-local, nonfaulting access to local SPM. Stores capture their source value
at admission. SST submits the real memory fragments and resumes the CPU before
their responses arrive. The instruction still pays ordinary issue/fetch costs.

After every timed response for an entry, SST snapshots load bytes while the
controller still owns their byte ranges. Entries retire in scalar issue order:
loads publish that snapshot; stores commit their captured bytes. External commit
then releases the ranges. The queue never rereads a load at an arbitrary later
QEMU boundary or changes backing memory before service completes.

Pending loads reserve their integer or FP destination. An instruction that reads
or overwrites it waits. Scalar operands/results of RVV and analog instructions
participate in these checks. FP32 loads retain RISC-V NaN boxing. Stores no longer
depend on live source registers after capture. Independent arithmetic, accesses,
and analog work can overlap with outstanding scalar memory service.

This unit issues in order, uses register scoreboarding, and retires memory entries
in order. It does not add speculative memory execution, register renaming,
store-to-load forwarding, or a data cache. Controller ordering preserves partial
byte overlap and interactions with vector accesses and other SPM clients.

## Synchronization and fallback

The existing instruction-fetch rendezvous carries a scalar dependency mask.
SST resumes a waiting instruction only after the required slots retire. A full
queue waits for a reusable slot; unrelated hardware continues progressing.
Shared slots have finite capacity, increasing tokens, and acquire/release state
publication. Instruction-budget changes affect host rendezvous frequency, not
simulated issue width or completion timing.

Fences, instruction fences, task markers, exits, and precise exceptions drain
older work. Misaligned accesses, page crossings, MMIO, stores to translated code,
and unsafe execution contexts retain the synchronized blocking path. The fast
path targets the current single-hart RV64 M-mode SPM model. Unknown instruction
semantics are conservatively drained. Guest buffer-ownership rules remain in
force for asynchronous users of received storage.

## Observations and verification

`<cpu>-slq.csv` records enqueue, issue, service completion, retirement, and stalls.
`<cpu>-slq-waits.csv` records actual CPU waiting intervals with `register`, `full`,
or `drain` reasons. These waits are not evidence of continuous SPM bank service;
the bank traces provide that evidence separately.

`RISCV_STATS` reports `scalar_load_store_queue_depth`, `slq_enqueued`,
`slq_completed`, `slq_peak_occupancy`, `slq_register_stalls`, `slq_full_stalls`,
`slq_drain_stalls`, and `slq_stall_cycles`. Task snapshots include cumulative
`slq_enqueued`, `slq_completed`, and `slq_stall_cycles` for measured regions.
Scalar requests also appear in the existing memory trace with `vector=0`.

The [scalar queue suite](../../tests/scalar-load-store-queue/README.md) checks
values, register and memory hazards, queue bounds, precise faults, disabled mode,
and timing invariance. Existing vector, analog, and network tests exercise the
default together with the other components. Rebuild both QEMU and the SST
components when updating this bridge protocol; incompatible binaries are rejected.
