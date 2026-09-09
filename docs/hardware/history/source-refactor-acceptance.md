# Refactoring acceptance

R0–R11, including the separately reproduced R5a/R5b correctness repairs, are
complete. This accepts the scoped hardware cleanup in `src2/`; it does not
switch the repository default from `src/`, validate compiler/model workloads,
or claim every possible parameter combination has been tested.

## What changed

| Before | After |
|---|---|
| Tile held CPU/replay, RX/TX, memory, analog and synchronization state | Dedicated owners with typed actions and narrow callbacks; Tile composes resources and handles SST lifecycle |
| Architecture parameter handling and checks spread across constructors | Central typed tile/router/NIC/global-RAM configuration and resolved snapshots; all 73 tile defaults checked |
| Repeated address and clock arithmetic | Shared whole-range decoding, explicit clock domains and checked rounding/overflow |
| A 45-argument summary call | Named owned snapshots; resource counters retain their owners |
| CSV values lacked availability and unit metadata | Compatible CSV plus versioned JSON with units, domains, scope, availability, implementation identity and config references |
| Dead NIC helper/state, repeated checks, stale adapters and documentation | Evidence-based removal, shared diagnostic formatting and consistent formatting of extracted code |

`Tile.cc` is now 1,776 lines versus 8,824 in the preserved source snapshot.
Responsibilities moved into named modules; this is not a claim that 7,048 lines
of functionality were deleted. All CPU/TX/RX/global-DMA clients still share one
SPM timing model. T1/T2/T4 remain parameterized without multiplying banks or
NoC resources. Existing RX capability, ordinary RAM and analog paths remain.

## Final acceptance gates

All structural/measurement comparisons below use the corrected, pinned R9
checkpoint, not the known-buggy original timing:
`build/src2/baselines/r9-1788565130287235401/install`.

| Gate | Result | Evidence |
|---|---|---|
| Full hardware matrix | 26 groups, 52 passing arms; zero counter and ordered-observation differences | [comparison](../../../build/src2/regressions/1788566020982317699/comparison.json) |
| Focused TX lanes/directions/banks/FIFO, modeled TX/SPM, RX clocks, one-vector RVV/SPM | 7 groups, 14 passing arms; zero differences | [comparison](../../../build/src2/regressions/1788566022187560292/comparison.json) |
| Configuration | All 28 checks pass, including the 73-field default golden and invalid/overflow inputs | `/tmp/golem-src2-configuration-moe7527w` |
| Host components | All 12 groups pass, including capture cancellation and writer golden/schema tests | [log](../../../build/src2/final-host-tests-with-validator.log) |
| Owner lifetime/memory safety | AddressSanitizer and UndefinedBehaviorSanitizer pass | [log](../../../build/src2/final-sanitizer.log) |
| Global-DMA deadline oracle | 13 corrected cases pass at three CPU clocks | [results](../../../build/src2/global-dma-clocks-2h85uvbk/results.json) |
| CPU delivery/SPM deadline oracle | 12 current + 12 reference cases pass; 48 ordered observation files match exactly | [results](../../../build/src2/cpu-memory-deadline-qe_dewxh/results.json), [comparison](../../../build/src2/final-deadline-comparison.json) |
| Serial/parallel QEMU capture | 9 identities / 18 simulations per arm; 264 summary/UART files match byte-for-byte across arms; 18 hardware report files match | [current log](../../../build/src2/final-ready-current.log), [reference log](../../../build/src2/final-ready-reference-retry.log), [comparison](../../../build/src2/final-ready-comparison.json) |
| Real measurement outputs | 712 summaries pass JSON/CSV, provenance, units, availability, actual clock-factor and histogram checks | [log](../../../build/src2/final-measurement-validation.log) |
| Validator corruption tests | 27 tests pass, including reordered/duplicate data, precision, missing config, invalid units and false zeros | [combined host log](../../../build/src2/final-host-tests-with-validator.log), `python3 src2/sst/tests/test_measurements.py` |
| Build/source-selection and comparison helpers | 7 + 6 host tests pass | `test-build-selection.py`, `test-comparison.py` |
| Actual running binary provenance | SST loads src2 Mittens, launches src2 QEMU, guest DWARF identifies src2 | [proof](../../../build/src2/checks/1788566061254333089/provenance.json) |
| Original source | All 133 original mapped file hashes unchanged | `python3 src2/verify.py`, `path-map.json` |

Final build:
[build-sst-1788565998619306020.json](../../../build/src2/build-sst-1788565998619306020.json).
Implementation input ID:
`sha256:f3303f45225c16aa99d080d20b8cb1371c7fa507e79bac4debd973dfac553f2b`.
Mittens binary SHA-256:
`02b231c017b9a0a94e3bde8dd6f735cdf99a93d528980347e35e7214bf3ef559`.
The final build log has no compiler warnings.

Pinned accepted binaries and source/build manifest:
[`refactor-complete-1788566243867779900`](../../../build/src2/baselines/refactor-complete-1788566243867779900/manifest.json).
Earlier failed runs remain preserved. The first final ready-set reference
invocation stopped before simulation because a pinned hardware-only install
does not include SST Core; its retry used the private dependency prefix already
created by the comparison runner. No pinned or reference binaries were changed.

## Intentional differences and limits

R5a fixes global-DMA CPU-cycle/tick conversion and deadline rechecks.
R5b prevents unrelated ordinary-memory responses from delivering future CPU
work or completing unfinished SPM service, and rejects stale CPU wakes. These
are real timing corrections, not merely renamed code. The pre-fix failures and
reviewed differences remain documented in [R5b](../../../src/sst/execution/R5b.md) and the
[task queue](../task-queue.md). Historical affected studies must be rerun; their
saved numbers have not been silently changed.

Measurements retain their honest limits: SPM conflicts count delayed beats,
service sums can overlap, TX opportunity histograms are sampled observations,
and some T2/T4 queue/blocked counters are incomplete. JSON marks unavailable
values explicitly rather than inventing new measurements. CPU issue cycles are
not elapsed execution. See the [measurement contract](../../../src/sst/profiling/MEASUREMENT_CONTRACT.md).

The tested machine is parameterized, not an unbounded claim about all future
architectures. Changing scheduling policy, adding RX lanes, changing data
ownership or changing hardware defaults remains a separate architectural task.
