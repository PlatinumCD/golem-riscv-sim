# Array pipeline regressions

Run `python3 -B src/tests/array-pipeline/run.py` from the repository root.
The harness builds real LLVM/QEMU/SST fixtures for a single array and leaves
reports under `tests/results/source-new-array-pipeline/<time>/`. `--case NAME`
selects a case; `--output PATH` selects its artifact directory.

Enabled and disabled cases cover dimensions 32/64 and VLEN 256/512. Five
different inputs check that a running MVM retains its input snapshot while
later inputs overwrite the load buffer. Input chunks contain at most five
floats, output chunks at most seven. Each output starts with an out-of-order
three-float read, repeats that range, then covers the remaining rows. This
checks unique coverage before FIFO retirement, output preservation, and vector
tail preservation. Guest and host both check numerical outputs.

`default-enabled` omits the architecture flag and verifies the default produces
the same numerical results, overlap, and phase timing as explicit enablement.

The enabled schedule submits the next MVM before draining its predecessor.
Trace checks prove Load and Store transfers, plus ordinary CPU SPM writes,
overlap array computation. Disabled runs retain sequential behavior. A
4,000-cycle compute cost makes the overlap observable; matched enabled runs
must finish their measured phase sooner. Additional cases cover instruction
budget one, zero compute latency, synchronous invalid-input status, rejected
weight mutation while jobs exist, fences, and exit with an unread async result.
Separate pending-work cases require `fence.i` and a marker without a preceding
guest fence to drain computation before their observable trace events.

A direct SST protocol driver fills both result slots, queues a third Execute,
and sends partial Stores behind it. Stores must bypass the blocked Execute;
repeating one row must not free a slot, and full unique coverage must admit the
third job. It checks Started-before-Complete ordering at both 4,000 and zero
compute cycles. The protocol fixture tests queue behavior that a CPU blocked
on a third scalar `mvm` could not drive itself.
