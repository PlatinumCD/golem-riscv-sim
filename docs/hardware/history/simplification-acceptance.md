# Targeted simplification: implementation and validation

All seven approved code changes are implemented and have passing focused checks.
**Final fresh-guest full-suite acceptance remains blocked**, not passed: concurrent
edits under `third_party/sculptor-mlir/runtime` broke the shared guest-runtime build
during the final run. Those files were not changed by this task.

## Changes

| Task | Implementation |
|---|---|
| S1 | Baseline pinning uses the common build/install resolver and output-ownership guard; it never falls back from an explicit missing build root. |
| S2 | `tests/run-group.sh` delegates to the authoritative runner. Hardware groups share its allowlist, timeouts and evidence. Compiler/models remain explicit opt-ins. |
| S3 | Removed the unreachable legacy branch from the SST element builder. |
| S4 | T1 and multi-TX share DMA beat/FIFO state, completion checks and SPM scheduling arithmetic. Their admission, frame ordering, retry policies and observation sites remain separate. |
| S5 | RX DMA/software claims share tag lookup, exact frame/word-count/ownership validation and claiming. DMA address/range checks and post-claim queue operations remain mode-specific. |
| S6 | Progress snapshots and diagnostics live in `profiling/tileProgress.*`. TX/RX value-only snapshot headers avoid importing SST component machinery into reporting. Tile retains observation sites, watchdog policy and shutdown. |
| S7 | Initial/runtime ready-set tasks share lease-guarded callbacks. Worker captures own transport/lease copies; initial validation and runtime pending-capture checks remain explicit. Local lookahead and ledger accounting are unchanged. |

No hardware resources, architecture defaults, scheduling policies, timing formulas
or counter meanings were changed. Counter ownership remains with the controllers.
Component checks remain under `src/sst/tests/`. No compiler/model suites or full
study sweeps were run. Eight fixed TX fixtures reuse the existing study guest.

## Passing checks

| Check | Result / evidence |
|---|---|
| Canonical path/ownership selection | 10 tests pass; `python3 -B src/test-build-selection.py` |
| Baseline capture | 4 tests pass; real override capture at `build/src/infrastructure-1788821292/baselines/simplification-path-check-1788823503975785751/` |
| Runner and comparison semantics | 13 runner + 8 comparison tests pass, including final progress comparison without host wall time |
| Public group delegation | `network/mesh-3x3` passes: [results](../../../build/src/test-runs/1788823564671476499/results.json) |
| TX stage | Eight fixed T1/T2/T4, shared-link, bank-offset and minimal-FIFO fixtures pass on both binaries, with zero counter/ordered-observation differences: [comparison](../../../build/src/regressions/1788823695987664467/comparison.json) |
| RX stage | Software payload, delayed cross-source DMA ordering, receive-head blocking and RAM/SPM deployment checks pass on both binaries with zero differences: [comparison](../../../build/src/regressions/1788823778410864405/comparison.json) |
| Progress stage | Standalone field/format golden test passes; CPU timing and TX fixtures match, including deterministic final progress fields: [comparison](../../../build/src/regressions/1788824036825369641/comparison.json) |
| Final host verification | All current host groups pass, including the nine explicit capture modes and replay/deadline checks: [log](../../../build/src/simplification-final-host.log) |
| CPU lifetime safety | Replay plus all nine capture modes pass under AddressSanitizer and UndefinedBehaviorSanitizer: [log](../../../build/src/simplification-sanitizers.log) |
| Final serial/parallel capture | Nine serial/parallel identities, 18 simulations per hardware arm; all 132 summary files and 132 UART logs per arm match exactly, with zero observation differences and 132 valid candidate summaries: [comparison](../../../build/src/simplification-capture-comparison.json) |
| Configuration | All 28 checks pass on both binaries; six positive hardware observations match: [comparison](../../../build/src/simplification-config-comparison.json) |

The capture comparison explicitly replays identical saved guest ELFs from
`build/src2/final-ready-current/tests/qemu-ready-set/`; their hashes are checked
before/after. This validates the changed hardware/callback implementation, **not**
the concurrently edited guest-runtime sources. The optional
`MITTENS_READY_SET_GUEST_DIRECTORY` diagnostic override is documented in the test
runner; its default still builds fresh guests.

The configuration comparison initially exposed random temporary-directory names
as false path differences. Runner-isolated configuration tests now use stable
names within their unique case root. No hardware records or fields were excluded
to obtain equality; standalone configuration runs still use unique temp roots.

## Blocked full gate

The [final 31-case paired run](../../../build/src/regressions/1788824180343567458/comparison.json)
is **FAIL**: each arm reports 5 passing and 26 failing cases, and both runtime
pre-builds fail. Shared runtime files changed during the run. Captured errors
include unused `runtime_` parameters in `deployment_receive.cpp:155` and `:172`.
The [later public subset](../../../build/src/simplification-public-host/test-runs/1788824465594694982/results.json)
also remains **FAIL** overall, even though host, configuration, network timing and
profile-validation entries pass: its runtime pre-build fails on constructor
initializer ordering in `deployment_lifecycle.cpp:56`.

These are failed build/integration attempts, not successful hardware validation.
They and their logs remain preserved. The all-suite comparison also recorded the
temporary-path differences described above; their dedicated follow-up passes.

Once the separate runtime work is buildable, run:

```bash
bash tests/run-all.sh --suite hardware
python3 -B src/regression.py \
  --reference-install build/src/baselines/simplification-before-1788823346684696712/install
bash src/sst/tests/run-qemu-ready-set-sst-test.sh
```

Do not close the SV acceptance task until these fresh-guest checks pass. The
pre-existing host dependency can still fail even though no Sculptor compiler or
model is being built: hardware test guests use its shared runtime library.

## Preserved implementation

- Before: `build/src/baselines/simplification-before-1788823346684696712/install`.
- Final SST build: [manifest](../../../build/src/build-sst-1788824112550218096.json),
  [warning-free log](../../../build/src/simplification-s7-build.log).
- Implementation ID: `sha256:51be76d77a9a1433bf29a3fc238e6af479f6b034da5ab2a33e55ac606455ae85`.
- Saved final hardware (implementation checkpoint, not full-suite acceptance):
  `build/src/baselines/simplification-implementation-1788824299310719468/install`.
- No staging or commits. Git index SHA256 remains
  `50f81be01c2dea35cc13f4c556d852836ff9f9fd7e803e912ffe73494b1121cc`.
