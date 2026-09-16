# CPU execution and clocks

| File | Responsibility |
|---|---|
| [cpuExecutionController.cc](cpuExecutionController.cc) | Consume stops, charge instruction work, and dispatch timed actions |
| [cpuExecutionLedger.h](cpuExecutionLedger.h) | Incremental scalar/vector issue accounting across grants |
| [qemuProcess.cc](qemuProcess.cc) | Launch and stop the managed QEMU process |
| [qemuCaptureCoordinator.cc](qemuCaptureCoordinator.cc) | Coordinate capture of QEMU stop records |
| [qemuCaptureExecutor.cc](qemuCaptureExecutor.cc) | Run host capture tasks and retain deterministic completion ordering |
| [clockDomain.h](clockDomain.h) | Checked conversions between resource cycles and SST time |

The CPU executes from SPM with single-issue, instruction-ordered replay. Resource timing
belongs to memory, network and analog controllers; this directory coordinates
when the CPU may continue.

## Concurrent host capture

`qemu_capture_workers` is the only supported capture-concurrency setting.
The coordinator, dispatch events, executor, and `MITTENS_QEMU_CAPTURE_WORKERS`
report use capture naming. The separate local-lookahead implementation remains
disabled and does not enable or configure this path.
The previous `MITTENS_RUNTIME_QEMU_READY_SET` log label is replaced by
`MITTENS_QEMU_CAPTURE_WORKERS`; the reported field names are unchanged.

`qemu_capture_workers=1` retains synchronous capture. Values 2–64 opt into
concurrent response collection for QEMU instances ready at the **same SST
timestamp**. This is a host-performance setting, not additional modeled cores,
DMA channels, or memory bandwidth. The current implementation requires one SST
rank and one SST event thread.

Ordinary same-time events enqueue capture requests. A later-priority dispatch
collects their responses through a fixed-size worker pool. Workers only operate
the QEMU transport; validation, accounting, and device actions stay on the SST
thread. Responses are committed in submission order, not host completion order;
each commit retains the ordinary modeled delay. A single-request dispatch uses
the direct path to avoid worker-pool overhead.

Only the first queued request at a frontier schedules the shared dispatch event.
The dispatch still services live peers if its original tile has stopped;
cancelled endpoints are removed and completion callbacks check their leases.
The complete capture batch is validated and published under one queue lock,
then workers are awakened after unlocking. The collector is notified when all
submitted tasks finish, including a partially submitted batch being discarded.
These changes remove host scheduling work, not guest fetches or modeled events.

This is conservative frontier concurrency, **not** unrestricted asynchronous
simulation. SST still waits for the complete same-time capture group before
advancing to a later time. It overlaps independent host responses without
crossing unresolved dependencies. No instruction-fetch or memory events are
removed, and lookahead/batching remain disabled. Short or poorly aligned work
can be slower because dispatch and worker wakeups add host overhead.

`tests/concurrent_capture.py --element-library <library-directory>` rebuilds
SPM/I-cache guests and compares 1/2/4 workers on aligned and mixed instruction
streams, including RVV accesses, instruction invalidation, and traps. It checks
exact per-tile traces, DMA records, and summaries (excluding host timestamps and
the PID-dependent resolved-configuration reference). The executor unit test
also forces a slow response to wait for a fast peer and checks deterministic
commit order. Results are written to `tests/results/concurrent-capture/`.
The normal hardware suite includes this regression, including a network
send/acknowledgment case and exact router-statistics comparison.

Capture durations summed over workers overlap: they are resource-time totals,
not elapsed host runtime. Use the runner's host wall time for speedup comparisons.
Do not add those durations to simulated cycles or other resource-stall totals.

The `MITTENS_QEMU_CAPTURE_WORKERS` report also exposes main-thread elapsed
`host_dispatch_ns`, `host_submit_ns`, and `host_collect_ns`. Submission and
collection are portions of dispatch: **do not add them to dispatch again**.
Collection includes waiting for workers and validating their captured events;
it is not a pure QEMU-execution measurement. Dispatch excludes committing those
events, normal SST event handling, startup, and shutdown. Compare it with the
runner's measured wall time to locate host overhead.

## Cache-hit register segments

`instruction_fetch_segment_size=1` retains one fetch handoff per instruction.
Values 2–16 let SST approve a short sequence of already-cached, 32-bit RV64I
integer register instructions in one handoff. This changes host transport, not
the modeled processor or cache. Rebuild QEMU and SST together: the bridge
protocol includes the proposed and approved instruction counts.

QEMU proposes a straight-line sequence within its remaining instruction grant
and one mapped page. Only nontrapping integer ALU operations are eligible;
memory, branches, CSRs, fences, compressed instructions, floating-point and RVV
instructions keep their individual handoffs. The CPU must be in machine mode
with interrupts masked, no active memory-protection rules, and without debug
triggers, single stepping or breakpoints. SST refuses
grouping while local DMA, analog work, or receive activity might write SPM.

SST performs **every** approved I-cache lookup through the existing cache,
updating replacement state and charging the full hit latency. A miss ends the
group before the missing instruction; the ordinary miss path still charges its
SPM fill. QEMU executes the approved instructions in its usual one-instruction
translated blocks and checks their addresses and opcodes. The CPU ledger
reconciles the approved issue cycles against actual retired instruction counts.

`tests/fetch_segments.py --install <install-root>` compares grouped and
ungrouped runs, including short grants, non-unit hit latency, eviction, code
replacement, DMA-written code, faults, RVV, and warmed integer code with an
instruction-count debug trigger configured. Simulated timing, instruction counts,
cache statistics and device counters must match. Only transport event
and host snapshot counts may differ. Results go to `tests/results/fetch-segments/`.
This conservative mode does not group arbitrary guest code and is opt-in until
workload-specific correctness and wall-time comparisons justify enabling it.

## Clock domains

`Timing::Ticks` is absolute SST core time; its unit is the configured SST
timebase, not an assumed nanosecond. `Clock<Domain>` obtains its tick period
from SST's actual `TimeConverter`.

- CPU instruction accounting and CPU-facing SPM service use CPU cycles.
- Shared-memory DMA service uses its controller clock.
- SPM-targeted RX/TX use the common SPM timing model in CPU cycles.
- Analog progress is the number of complete analog cycles elapsed (floor).
  Analog wakeups use checked analog-cycle-to-tick conversion.
- NoC completion work converts link cycles to ticks; accumulated packet transit
  time is not an extra wall-clock interval to add to elapsed execution time.

Typed cycle values cannot implicitly cross domains. Boundary conversion goes
through `Ticks`. Multiplication and addition reject overflow; ceil conversion
does not use the overflowing `(value + divisor - 1)` idiom. Guest ABI counters
stay integer-valued; types constrain the simulator-side conversion points.

Service cycles and stall sums may overlap. Neither should be summed into elapsed
runtime without a disjoint-interval proof. The progress watchdog is host wall
time; its polling interval is one million CPU cycles, 1 ms only at 1 GHz.

See the [timing model](../../../docs/timing-model.md) for formulas and
[measurement rules](../profiling/MEASUREMENT_CONTRACT.md) for counter meanings.
