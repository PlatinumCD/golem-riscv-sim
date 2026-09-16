# DMA and mesh progress: findings

This is the completed evidence report for `goals.md`. Times below are simulated
CPU cycles unless explicitly labeled as host seconds.

## 1. Idle waiting — complete

Evidence: `tests/results/dma-diagnostics/1789534804566675109/` (60-case baseline;
idle subset: two policies, four quanta, three repeats).

| Policy | Elapsed cycles | Guest instructions | Queries |
|---|---:|---:|---:|
| Blocking | 18,477 | 39 | 0 |
| Polling | 18,521 | 18,510 | 1,230 |

The simulated values repeat exactly at quanta 64, 256, 1000, and 4096. RAM service
is 18,440 cycles in both arms. Polling adds guest execution without materially
accelerating the transfer. It is a waiting-policy cost, not a RAM bandwidth gain.
At quantum 1000, whole-run bridge events are 460,259 versus 480,316. These counts
include post-region validation and are explicitly not region-only handoff counts.
Per-run host times are recorded in `results.json`; the initial sweep overlapped
builds/other runs, so its wall-time ratios are not isolated host speedups.

## 2. DMA plus useful computation — complete

Evidence: `tests/results/dma-diagnostics/1789535365266321361/` (30 cases: five
compute sizes, two policies, three repeats). One 64 KiB DMA, no mesh traffic.
The same register-only loop executes eight adds plus decrement/branch per
iteration; generated disassembly confirms no loads or stores in that loop.
Registers are initialized before the timing marker and checked afterward.

| Compute iterations | Blocking cycles | Overlapped cycles |
|---:|---:|---:|
| 0 | 18,483 | 18,526 |
| 128 | 19,759 | 18,530 |
| 512 | 23,600 | 18,531 |
| 2,048 | 38,961 | 20,572 |
| 8,192 | 100,400 | 82,011 |

DMA proceeds without a blocking wait command. Blocking adds transfer time to
computation; submitting first permits overlap. Once computation is long enough,
the final query sees a completed DMA. The CPU/DMA overlap mechanism is working.
Checksum, payload, retained-completion, capacity, and identity tests pass.

## 5. Execution-quantum sensitivity — complete; simulator defect fixed

Evidence: baseline `1789534804566675109`, corrected replay
`1789535332327804596`, under `tests/results/dma-diagnostics/`. Sixty runs in
each cohort, using the exact same guest ELF hashes and hardware settings.

| Quantum | Old blocking cycles | Old polling cycles | Corrected blocking | Corrected polling |
|---:|---:|---:|---:|---:|
| 64 | 26,595 | 18,524 | 24,803 | 18,514 |
| 256 | 50,723 | 32,556 | 24,803 | 18,514 |
| 1,000 | 82,035 | 127,038 | 24,803 | 18,514 |
| 4,096 | 278,563 | 520,229 | 24,803 | 18,514 |

Every value repeats exactly three times. In the corrected run, relative packet
injection/arrival timestamps and ordering also match across all four quanta.
The old scalar NIC write only yielded to SST when its bridge ring filled.
Sparse sends remained unpublished until another boundary serviced the bridge.
`src/qemu/devices/mittens-nic/mittens_nic.c` now synchronizes every scalar send
at its instruction boundary. The result is quantum-independent in this tested
matrix; this is not a claim of invariance for every possible workload.

This was a simulator visibility defect, not a physical NoC bandwidth limitation.
The previously reported cooperative throughput regression is invalid as a clean
hardware conclusion. Corrected polling throughput is 1.34 times the blocking
baseline for this workload. RAM service remains unchanged at 18,440 cycles.

## 4. Polling frequency — complete

All five intervals ran three times in the corrected replay at quantum 1,000.

| Query every N iterations | Total cycles | DMA queries | Guest instructions |
|---:|---:|---:|---:|
| 1 | 18,514 | 660 | 18,501 |
| 4 | 18,552 | 241 | 18,537 |
| 16 | 18,571 | 72 | 18,556 |
| 64 | 18,937 | 20 | 18,922 |
| 256 | 19,631 | 6 | 19,616 |

Fewer DMA checks do not eliminate idle instructions; they increasingly delay
completion detection. Separate diagnostic runs with handling markers and PC-level
retirement attribution (`1789535838786195925`) explain the remaining work:

| Query interval | DMA-query instructions | Mesh-status polling | Loop bookkeeping |
|---:|---:|---:|---:|
| 1 | 7,080 | 316 | 9,800 |
| 4 | 2,592 | 379 | 14,151 |
| 16 | 777 | 424 | 15,798 |
| 64 | 227 | 437 | 17,153 |
| 256 | 73 | 441 | 17,989 |

These instrumented instruction counts are not from the uninstrumented timing
table above. All retirements reconcile, including markers, submission,
acknowledgment, and mesh handling. Reducing queries largely replaces them with
bookkeeping; it does not make a busy loop sleep.

## 3. DMA plus mesh responses — complete

Evidence: `1789535513412064306` (72 policy/quantum/repetition cases),
`1789535871936531791` (six detailed attribution cases), and the frequency
attribution cohort. Each trace links request arrival, software handling, response
issue, and response arrival in order. The audit verifies 576 such chains.

| Policy | Total cycles | Guest instructions | First request arrival to reply issue | First response arrival after region start |
|---|---:|---:|---:|---:|
| Blocking | 24,805 | 6,361 | 18,461 | 18,526 |
| Polling | 18,517 | 18,504 | 69 | 120 |
| Event-driven | 18,515 | 2,596 | 75 | 122 |

Blocking prevents the CPU from responding until its DMA completes. Both other
policies respond while DMA is active, completing the same 64 exchanges and DMA
about 1.34 times faster. RAM service is still 18,440 cycles. This is independent
communication progress, not increased memory bandwidth. All simulated values
and relative packet event ordering repeat exactly over four quanta and three
repetitions. Detailed handling markers are only in the diagnostic cohort, not
the timing table.

## 6. Event-driven waiting — complete

The combined DMA-or-mesh wait is implemented in the paired `install/dma-event`
build. It checks readiness at the instruction boundary, sleeps when neither
condition holds, and wakes for either exact DMA completion or available mesh
work. A mesh wake does not consume the message. Complete includes functional
data movement, and the completion remains retained until acknowledgment.

Unit tests cover readiness before arming, a mesh wake while a DMA deadline is
pending, DMA-only wake, and retained completion. Integrated tests verify data,
all eight completion slots, rejection at capacity, stale execution/token
identity, outgoing-source reuse, and repeated completion queries.

Compared with polling, event-driven mesh handling uses **86.0% fewer guest
instructions** at effectively the same elapsed time. With no mesh traffic,
event waiting uses 78 instructions versus polling's 18,513. The combined wait
preserves useful progress without spending the idle interval executing polls.
No default application runtime policy has changed.

### Host time: not a demonstrated speedup

The additional sequential cohort `1789535936111717223` ran three repetitions
per case at quantum 1,000. Times cover the entire process, including checks
outside the measured guest region. Load averages are saved with each case.

| Workload | Policy | Host seconds mean [min, max] | Whole-run bridge events |
|---|---|---:|---:|
| Idle | Blocking | 4.155 [3.593, 4.482] | 460,277 |
| Idle | Polling | 4.789 [3.647, 5.638] | 480,346 |
| Idle | Event-driven | 4.555 [3.533, 5.733] | 460,393 |
| Mesh | Blocking | 4.987 [4.832, 5.282] | 466,680 |
| Mesh | Polling | 4.287 [3.878, 4.647] | 479,765 |
| Mesh | Event-driven | 4.734 [3.677, 5.285] | 463,164 |

The ranges overlap substantially. Event waiting reduces instructions and
handoffs, but these samples do not prove a host speedup. Post-region verification
is substantial; a production-runtime host benefit remains unmeasured.

## Deliverables and interpretation

- [Repeated measurement tables](../../results/dma-diagnostics/report/measurements.md)
  include all nine cohorts, exact paths, cycle variation, and host variation.
- [Aligned measured timelines](../../results/dma-diagnostics/report/timelines.html)
  separate instruction issue, blocked CPU time, RAM service, and mesh transit.
  Register-only computation is included alongside the waiting policies.
- [Machine-readable audit](../../results/dma-diagnostics/report/checklist-audit.json)
  checks all 270 simulations, hardware equality, ELF/build hashes, exact input
  bytes, 24 instruction attributions, 576 handling chains, and 768 router services.

The six router-trace cases (`1789536519069916138`) use the same guest hashes and
produce identical elapsed times and instruction counts to the corresponding
host-repeat cases. `resource-intervals.json` in each case records RAM and router
queue/service/idle intervals. Each one-word packet uses one output service cycle;
all measured router queue waits after pipeline readiness are zero. RAM requests
queue for one cycle. Thus the long blocked response is a software wait, not a
busy network output. Endpoint transit includes propagation/pipeline time and
is explicitly not labeled link utilization. Idle intervals are complements of
service within the recorded window, not new counters invented for missing data.

| Finding | Responsible layer | Supported action |
|---|---|---|
| Sparse scalar sends depended on synchronization quantum | Simulator | Publish scalar sends at their instruction boundary; implemented and replay-tested |
| Blocking DMA delays unrelated mesh handling | Runtime interface/policy | Use token-specific cooperative progress or combined event wait; both validated |
| Less frequent queries mostly become bookkeeping | Runtime policy | Sleep when no work is ready rather than only throttling queries |
| Useful CPU work overlaps DMA service | Hardware model | Preserve overlap; this experiment does not justify adding RAM channels |
| Lower guest instruction count did not establish lower host time | Host simulation | Do not claim a production walltime gain from these samples |

The scoped core verification, combined-wait owner/controller unit tests,
scalar/17-tile DMA tests, and network pair/mesh regressions passed on the paired
event-wait build. The older `global_dma_clock.py` test has
an obsolete configuration (`memory_init_batching=true`) and was not counted as
passing; deadline behavior is covered by the new unit and integrated tests.
No whole-model inference result or universal quantum-invariance claim is made.

## Measurement rules

- Report elapsed cycles, instruction counts, host time, and resource activity separately.
- Missing data is null, never an assumed zero.
- Never add concurrent resource stalls as elapsed time.
- The old single-word request/reply throughput figures are not clean hardware
  bandwidth measurements: they contain a synchronization-quantum-dependent delay.
- Current results characterize synthetic workloads; no full-model speedup is claimed.
