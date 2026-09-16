# Cooperative global DMA

Does a tile service mesh requests while an unrelated shared-RAM DMA is pending?

Two adjacent tiles exchange 64 request/reply pairs. Tile 0 also reads one
64 KiB payload from shared RAM into SPM. Compare blocking wait, query on every
progress-loop iteration, query every 16 iterations, and event-driven waiting. Hardware is identical:
one 4 B/cycle RAM channel, 64-byte bursts, the same mesh links and 256 KiB SPM.
Only software waiting policy changes. This measures application round-trip
throughput, not peak mesh bandwidth or full-model inference throughput.

Run with a matching protocol-31 QEMU/SST installation:

```sh
python3 tests/memory/dma-cooperative/run.py --install install/no-legacy-init
```

Results, traces, executable hashes, and guest binaries go to
`tests/results/dma-cooperative/<run>/`. The timed interval covers submission,
DMA completion, and issuing all replies. `first_reply_cycles` measures the
first reply issued by tile 0, not its arrival at tile 1. Both tiles validate
their payloads. Host wall time covers the entire test, including verification.
Guest CPU cycles and retired instructions come from the measured task markers.

## Measured result

See [findings.md](findings.md) for repeated results, the scalar-send timing defect
discovered by the quantum sweep, and corrected measurements. The initial
throughput regression was contaminated by that simulator defect and must not
be presented as an architectural bandwidth finding. No full-model performance
result is claimed.

`diagnostics.py` provides baseline, compute, and event-driven matrices, an exact
guest-ELF replay option, and optional instruction attribution traces. Results go
to `tests/results/dma-diagnostics/<run>/`. `attribute.py` reconciles retirement
counts with PCs and debug frames; `report.py` produces tables and aligned timelines.

The host hardware verification suite and existing scalar/17-tile scratchpad-DMA
regressions passed. The global-DMA clock oracle now uses timed SPM boot rather
than the removed aggregate initialization path; scalar and batch cases passed
at 500 MHz, 1 GHz, and 2 GHz. Original diagnostic cohorts remain unchanged.

Correctness checks outside timing exercise all eight slots, retained completion,
unknown/retired identities, reuse across executions, outgoing source reuse after
completion, and readback. SST unit tests separately check the local-SPM deadline
and controller completion: neither alone is sufficient for Complete.

## Interface

`globalDMAQuery(execution, token)` returns Pending, Complete, or Error. It never
retires the request. Complete means SST controller and local-SPM service have
finished and QEMU has performed the functional copy. The first successful query
performs that copy; later queries do not copy again. The identity is scoped to
the issuing tile. Unknown and already acknowledged identities return Error.

`globalDMAAcknowledge(execution, token)` releases a completed request. Calling it
before observing Complete (or a retained functional-copy error) is an error and
does not retire anything. A caller may instead use the existing blocking wait,
which completes and retires in one operation. Query/ack are not allowed inside
a compiler-certified DMA macro tape.

Eight slots hold both pending jobs and retained completions. A full table rejects
submission without overwriting a slot. Acknowledgement or blocking retirement
frees a slot; a query does not. Do not reuse an identity before retirement. Use
distinct execution IDs for distinct invocations; reuse of the exact same pair
after retirement cannot distinguish a stale software reference to that pair.

DMA scheduling proceeds independently of wait commands. Deferred submit batches
are published at synchronization boundaries and before a query. Queries still
cross the QEMU/SST boundary and execute guest instructions: nonblocking does not
mean zero-cost.

`globalDMAWaitForEvent(execution, token)` checks the specified transfer and
incoming mesh readiness atomically on the SST event thread, then sleeps if
neither is ready. Complete includes functional copying and retains the DMA;
Pending means incoming mesh work is ready for software to service. It does not
consume that work. DMA controller completion, a local-SPM deadline, or incoming
mesh progress can wake the CPU. Already-ready work returns immediately. Use
`mesh_nic::wait_for_receive()` after the DMA is complete if only mesh work remains.

Keep an outgoing source unchanged and do not read/overwrite an incoming buffer
until Complete. Fence source writes before submission (`fence iorw, iorw`). The
query helper fences device accesses and subsequent guest memory operations.
Functional writes follow the existing global-DMA ownership contract; this is
not coherent shared-memory access by arbitrary concurrent writers.
