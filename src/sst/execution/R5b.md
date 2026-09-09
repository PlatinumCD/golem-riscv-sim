# CPU delivery and SPM retry deadlines

R5b is an intentional correctness change after the behavior-preserving R8
extraction. The original and pinned R8 implementations are not timing oracles
for the failing cases below.

## Failure and repair

A captured event was installed before its instruction delay elapsed. A buffered
RAM response or global-DMA notification could call the pending-event handler
early. The direct SPM path also used a scheduled/not-scheduled flag instead of
checking its completion timestamp. A notification could therefore complete an
unfinished SPM access. An old timer could subsequently advance a newer event.

The execution controller now tracks an absolute ready tick for every scheduled
instruction/replay/device delay. All pending processing and direct device/memory
completion entries check it. Scheduled CPU wakes carry revocable generations;
old wakes cannot act on newer work. Direct SPM service additionally records its
CPU step and physical completion deadline in its owner. This is not a new
latency or bandwidth parameter: it enforces the time already charged.

## Independent reproduction

`tests/cpu_memory_deadline.py` runs two cacheless tests at CPU 500 MHz, 1 GHz and
2 GHz with blocking-store and buffered-store configurations. No L1/L2 is added.
One test places 4096 register-only instructions before a fence; the other places
two long-latency SPM loads after an independent buffered RAM store. Guest payload
checks are separate from timestamp oracles.

Before: `build/src2/cpu-memory-deadline-lvo6xiim/results.json`.
After: `build/src2/cpu-memory-deadline-93s_js1a/results.json`.
All 12 corrected cases pass; the six buffered reference cases still fail,
the six blocking reference controls pass, and every guest payload passes.
Host tests also cover early direct completions, repeated retries, stale timers,
and shutdown at four tick/cycle factors.

At 1 GHz the broken instruction test finishes at tick 156000 although the
register instructions require at least tick 4143000. The broken SPM test
completes its first load at tick 150000 instead of waiting until tick 10048000.

## Existing hardware results

| Check | Outcome |
|---|---|
| 25 integration groups, both pinned and corrected hardware | Functional PASS |
| Component group rerun, both hardware arms | PASS, exact counters and observations |
| TX/SPM overlap, mixed RX clocks, one-vector SPM sweep | PASS, exact counters and observations |
| R5a global-DMA clock oracle | All 13 PASS |

The full comparison retains **20 summary differences and 12 ordered/report
differences** in two existing DMA tests. They are reviewed timing corrections,
not an exact-equivalence pass. All instruction, request, byte and SPM service
counts remain unchanged. For example:

| Scratchpad-DMA scalar test, 1 GHz | Before | Corrected |
|---|---:|---:|
| Guest instructions / issue cycles | 1720 | 1720 |
| Total finish time, CPU cycles from tick zero | 1084 | 1893 |
| SPM accumulated service cycles | 391 | 391 |

The old run finishes before its own accumulated guest issue cost. Scalar/macro
and the tested quantum variants now agree at 1893 cycles. Global-DMA contention
retains identical data and service costs but waits six additional CPU cycles for
an already-accounted instruction boundary. Wait traces and request timestamps
change accordingly. Historical affected timing results must not be relabeled
as corrected results without rerunning them.

Evidence:

- Build: `build/src2/build-sst-1788563853477972729.json`.
- Accepted pinned hardware (captured with candidate label):
  `build/src2/baselines/r5b-candidate-1788563863788379039/install`.
- Full pinned report: `build/src2/regressions/1788563882845986912/comparison.json`.
- Component rerun: `build/src2/regressions/1788564257661472140/comparison.json`.
- Full original report: `build/src2/regressions/1788563884018875526/comparison.json`.
- Original component rerun: `build/src2/regressions/1788564258855280625/comparison.json`.
- Focused exact report: `build/src2/regressions/1788563885222793281/comparison.json`.
- Global-DMA oracle: `build/src2/global-dma-clocks-ztfdeor0/results.json`.

The initial component compilation failures were caused by a missing host-test
link dependency while the new measurement writer was integrated. The runner now
links that writer; successful reruns use unchanged hardware binaries. Failed
reports remain preserved. No expected timing values were loosened to pass.
