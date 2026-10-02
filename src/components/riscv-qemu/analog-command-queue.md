# Analog command queue

`analog_command_queue_depth` enables a bounded CPU queue for `mvm.vset`,
`mvm.vl`, and `mvm.vs`. The default is four entries; depths 1–16
allow independent instructions to proceed after safe admission. Set the depth
to zero to disable the queue and use blocking transfers. It is separate
from `load_store_queue_depth` and `array_pipeline_enabled`. Scalar `mvm` still
returns at Complete, or at Started when array pipelining is enabled.

```python
cpu_parameters = {
    "load_store_queue_depth": 16,
    "analog_command_queue_depth": 4,
    "analog_command_queue_bytes": 16384,
}
```

Each entry owns a token, instruction PC, operation, array, element offset/count,
physical vector-register group, and up to 1024 payload bytes. The additional
active-payload byte limit accepts multiples of four from 1024 through 16384.
Input and output payloads both consume credit. Completion releases active-byte
credit; a physical slot remains owned until QEMU consumes its completion. Slot
count therefore bounds completion storage as well as live commands.

## Admission and register dependencies

SST submits a command and waits for a backend admission response. `Guaranteed`
means all necessary state checks and reservations establish that completion
cannot produce a guest fault; only then may the issuing helper return early.
This consumes the existing two-cycle command/response link round trip. `Busy`
retries the same token while the CPU holds its current instruction. `Accepted`
without a guarantee keeps that instruction blocked until Complete or Error.

Program/Load take functional source snapshots, but modeled register reads are
streamed over the timed link. Writes to their complete physical register group
wait until Captured: all source bytes have been consumed. Programming delay may
continue after source release. Store reserves its full destination group until
Complete; dependent reads and writes wait. Completion uses the original saved
VL, destination and group even if subsequent vector configuration changes.
Only active FP32 elements change, preserving ordinary and fractional tails.

These hazards cover both analog commands and ordinary RVV/LSQ operations.
Captured memory stores keep immutable source bytes and do not pin registers.
Unknown instructions conservatively drain pending work. Traps, fences, task
markers and exit drain older commands. Errors remain precise: an unguaranteed
command fails at its issuing instruction, with older work complete before the
trap handler begins. A failure after Guaranteed is a simulator protocol error.

Queue progress and completion checks reuse instruction-fetch rendezvous, with
explicit waits for queue capacity and dependencies. Completions update their
own slots rather than the current instruction's bridge payload. An asynchronous
callback wakes only an armed CPU wait; it cannot bypass issue or cache latency.
Instruction budgets change host synchronization frequency, not modeled timing.

## Bandwidth and array ordering

The model assumes dedicated analog register streaming ports: one read and one
write port, each bounded by `VLEN / 8` bytes per cycle across the tile. Existing
global array-link budgets enforce service: shared duplex combines both directions
under one budget; independent duplex permits one budget in each direction.
Host-side payload copies create no extra modeled bandwidth. This is not a common
banked register-file arbiter shared with all RVV ALU and LSU traffic.

The array retains four accepted command slots per array, one transfer engine,
its finite `array_inflight_bytes` buffer, one compute engine and two pipelined
result slots. Queued weight/input updates precede dependent computations.
Stores reserve the correct oldest output and its row coverage; output reads
may still bypass blocked Execute commands to release result capacity. Initial
program coverage includes admitted chunks, preventing an early guarantee from
becoming a late programming-epoch fault. Uncertain output availability keeps
the conservative blocking path. There is no direct SPM–array connection.

## Observations

`<cpu>-asq.csv` records enqueue, issue, Busy, Guaranteed/Accepted, Captured,
Complete/Error, and stalls. Entries include backend token, QEMU queue token,
slot, PC, operation, array, element range, register mask, occupancy and active
payload bytes. Occupancy includes a command whose admission is waiting or
retrying; completion rows exclude the completed entry.

`<cpu>-asq-waits.csv` records exact register/full/drain wait intervals. Full
includes slot and active-byte limits. The `asq_` CPU counters report enqueue,
completion, peak occupancy/bytes, Busy responses, guarantees, blocking fallbacks
and dependency/capacity stall cycles. Admission round trips are visible between
issue and Guaranteed/Accepted and are not included in `asq_stall_cycles`.
Overlapping SPM, array and CPU activity must not be summed as serial latency.

## Build and validate

The bridge ABI is version 34: build matching QEMU and SST components. Dedicated
paths allow an existing campaign to retain its original binaries:

```bash
python3 -B src/components/riscv-qemu/build_qemu.py --output build/src-qemu-analog-queue
python3 -B src/build.py --output build/src-analog-queue
python3 -B src/tests/analog-command-queue/run.py
python3 -B src/tests/analog-command-queue/admission.py
python3 -B src/tests/analog-command-queue-backend/run.py
```

The [directed suite](../../tests/analog-command-queue/README.md) checks hazards,
source capture before programming completion, destination tails, queue/byte
pressure, multi-array results, precise faults, fences and instruction-budget
replay. Its admission extension exercises backend Busy retries and Accepted-to-Error
handling. Backend tests independently check state reservations, capture timing,
finite capacity, Store bypass and aggregate link bandwidth.

The original matched experiment used 12 workload configurations and 1000 MVMs
per batch. ASQ depth 4 reduced resident runtime by up to 39.12% and programming
time by up to 41.02%; small arrays already at the 100-cycle compute limit changed
only at batch boundaries. These are measured results for those kernels and
resource settings, not general speedup guarantees. The committed validation
summary and reproduction commands accompany the directed tests.
