# Simulator task queue

## Fixed-work local versus two tiles — Q1

Status: COMPLETE. Same 4-KiB useful workload and final result on tile A.

- [x] Implement two strategies, fixed hardware, explicit coordination and full verification.
- [x] Pass corner preflight and paired-work / no-transfer-overlap validators.
- [x] Execute 20 primary cases; report measured runtime and benefit-minus-cost.
- [x] Update decision Q1 with actual comparison and timelines; desktop/mobile QA.
- [x] Remove the earlier supporting characterization module from the webpage only.

Study: `studies/compute-communication/local-vs-two-tiles/`. Existing sideband
barriers separate distribution, parallel compute and collection; their modeled
delays are included and are not described as NoC control bytes.
Evidence: primary `1788982764967821228` (20/20), refinement
`1788982821435767930` (4/4). Both retain unchanged source/binary hashes during
execution. K=12 loses 27 cycles; K=13 saves 37. Three study tests and two catalog
tests pass. Desktop/mobile graphs, controls and timelines visually checked;
no JavaScript errors or horizontal overflow. Old supporting report is absent
from the webpage; its underlying study data were not deleted.

## Computation–communication balance — Question 1

Status: COMPLETE. Approved matrix: 10 K values × 11 payload sizes × four link
widths (32/64/128/256 bits), 440 cases. No contention or overlap.

- [x] Register fixed hardware, timing boundaries and calibration/held-out split.
- [x] Implement real RVV compute and modeled SPM TX/RX transfer, O3 guest build.
- [x] Pass corner preflight, instruction/byte conservation and endpoint checks.
- [x] Execute all 440 cases with unchanged hardware and retained provenance.
- [x] Fit phase costs on calibration only; report held-out error and crossover brackets.
- [x] Publish Q1 measurements, equations, mesh and interactive sweeps; browser QA.

Study: `studies/compute-communication/balance-crossover/`.
Results: `studies/results/compute-communication/balance-crossover/<run-id>/`.
Validated preflight: `1788980480692242992` (8/8). Full sweep started:
`1788980498902556397`. Larger payloads use legal 16-KiB submissions; adaptive
8–32-word packetization is validated, not incorrectly assumed to be constant.
Full run: 440/440 valid; processing passes 320/320 held-outs (max 2 cycles),
communication 155/320. The global communication affine model is explicitly
rejected; measured crossover brackets remain the evidence. Six study tests and
two catalog tests pass. Desktop/mobile controls and diagrams verified with no
JS errors or horizontal overflow. FINDINGS.md documents boundaries and limits.

## Remaining theory category — Questions 3–5 (2026-09-09)

Status: COMPLETE — Q3, Q4 and Q5 implemented, executed and published. Order: Q3 → Q4 → Q5. Complete all six stages for one question
before moving to the next. Mark tasks ACTIVE before starting and record
evidence before checking them off. Q1 and Q2 already have completed studies
and dashboard reports. No additional architecture changes or application runs.

### Q3 — How do message size and arrival order divide the contention cost?

Study: `studies/theory-3/`. Page: `03-service-order.html`.
Existing Q6 has related measurements: explicitly audit/reuse suitable machinery,
but do not relabel those old results as a fresh Theory 3 execution.

- [x] **Find the theory.** Read the textbook arbitration/allocation sections
  and primary sources. Establish eligibility, scheduling unit, output ownership
  and round-robin assumptions. Distinguish fairness between inputs, flows and
  bytes; investigate Theory 2's unequal overload allocation without assuming
  its cause.
- [x] **Write the theory.** Add concise explanation, equations, symbol definitions,
  assumptions and precise citations to Q3 before the experiment. Separate
  textbook claims, ideal-sharing assumptions and our own derivations.
- [x] **Design `theory-3`.** Register message-size split × arrival spacing/order,
  solo and separated controls, exact routes/shared inputs, fixed packetization,
  prediction inputs and held-out cases. Document reuse from Q6 and the fresh
  evidence needed. Predict from release times and policy, not target-run grants.
- [x] **Implement.** Build traffic generation, the policy predictor, packet/queue
  observations, strict validators and CSV/JSON/report outputs. Verify payloads,
  readiness and service boundaries. Label whole-message FIFO and equal sharing
  as comparison models, not the actual policy.
- [x] **Execute and analyze.** Run unit tests, preflight, then the registered
  sweep and held-out checks. Record waiting, per-message completion, slowdown,
  batch makespan and actual service order. Preserve prediction failures;
  do not retune using held-out outcomes.
- [x] **Update the webpage.** Replace the design-only cross-link with theory →
  experiment → equations/predictions → measurements → qualifications → answer.
  Add the actual mesh, interactive service timeline, size/arrival sweeps and
  prediction errors. Link fresh evidence and relevant Q6 context. Build and
  visually inspect desktop/mobile rendering and controls.

Q3 evidence: `studies/results/theory-3/1788974756622554930`: 179/179 valid,
24/24 held-out cases pass, zero packet prediction error, unchanged sources/
binaries. Five local tests pass; desktop/mobile controls render without errors
or horizontal overflow. Initial analyzer-import failure is preserved separately.

### Q4 — How does sustained traffic increase latency near saturation?

Study: `studies/theory-4/`. Page: `04-load-latency.html`.

- [x] **Find the theory.** Read textbook performance/queueing treatments and
  primary sources for utilization, waiting and arrival variability. Verify
  M/G/1/Pollaczek–Khinchine assumptions against finite buffers, round-robin
  service and backpressure; do not force an incompatible formula onto Golem.
- [x] **Write the theory.** Explain offered demand, service rate, utilization,
  waiting and residence time with equations, units and citations. Explain why
  delay grows near saturation and why overload cannot be summarized by an
  assumed stationary mean while demand keeps accumulating.
- [x] **Design `theory-4`.** Register offered load × regular/Poisson/bursty
  arrivals at matched mean demand, densely sampling near saturation. Freeze
  routes, packetization and hardware. Define readiness before admission,
  warmup/windows/drain, independent seeds, convergence checks, uncertainty
  estimation and calibration/holdouts if fitting is needed. Cite Theory 2's
  capacity baseline explicitly rather than treating it as new evidence.
- [x] **Implement.** Build reproducible generators, source/network latency and
  queue instrumentation, runner and validators. Preserve offered, admitted and
  delivered rates separately. Account for outstanding/censored packets so
  latency summaries cannot silently omit unfinished work.
- [x] **Execute and analyze.** Run preflight and the registered sweep; check
  stationarity and seed/window convergence. Fit only unknown terms on declared
  calibration data and score held-outs. Report mean/tail latency with uncertainty
  below saturation; backlog growth and finite-window statistics above it.
  Retain rejected approximations.
- [x] **Update the webpage.** Preserve theory and add paired latency/throughput
  curves, arrival-pattern controls, source waiting versus network residence,
  queue growth, uncertainty and model residuals. State the valid operating
  region and limitations. Build and visually inspect desktop/mobile views.

Q4 evidence: `studies/results/theory-4/1788975326233742728`: 348/348 valid,
52/52 subcritical stochastic group references compatible, but only 6/8 longer
window comparisons pass. Retained: 191 half-window warnings and 32 packets
completing after the guard. No censored packets; no fitting or hardware changes.
Desktop/mobile latency and queue views inspected. Equilibrium limits are visible.

### Q5 — Can congestion delay a message whose downstream path is free?

Study: `studies/theory-5/`. Page: `05-indirect-blocking.html`.

- [x] **Find the theory.** Read textbook and primary treatments of head-of-line
  blocking, wormhole backpressure and buffer/channel coupling. Identify the
  shared queue/dependency that can cause interference despite a free output.
- [x] **Write the theory.** Explain the blocked head, victim, shared upstream
  input and free downstream branch. Include equations and citations; label a
  measured causal contrast separately from a predictive formula. Do not assume
  virtual channels exist.
- [x] **Design `theory-5`.** Register blocker intensity/duration × victim spacing,
  no-blocker controls and shared-input versus independent-input controls. Match
  victim route/payload and hardware. Verify legal geometry and distinguish
  direct competition from indirect coupling. Specify observations and pre-run
  inputs for a queue-aware prediction or bound, with validation cases held out.
- [x] **Implement.** Build traffic controls, runner and validators for queue order,
  head eligibility, credits, output ownership and unused output opportunities.
  Reuse existing instrumentation where possible; architectural changes such as
  new virtual channels require separate approval.
- [x] **Execute and analyze.** Run preflight and the sweep. Require evidence that
  victim waiting overlaps a blocked head while its requested output is otherwise
  available before claiming causality. Compare direct-link-load predictions
  with measured interference; validate any queue-aware model on held-outs.
  Preserve negative findings as well as positive ones.
- [x] **Update the webpage.** Preserve theory and add the actual shared-input/
  diverging-path mesh, queue/credit/service timeline, victim slowdown sweeps
  and prediction comparisons. Explain what direct channel load misses—or why
  it suffices. Link evidence; visually inspect desktop/mobile interactions.

Q5 evidence: `studies/results/theory-5/1788976060317819647`: 146/146 valid,
30/30 held-out predictions pass, zero cycle error, unchanged source/binary
hashes. Independent-input victim remains 60 cycles; shared-input extra delay
reaches 999 cycles. Thirty-eight cases have conservative queue-level witnesses.
The one recovery coefficient was frozen from separate preflight evidence.

Final checks: 16 local study/catalog tests pass. Dashboard compiles 27 questions;
Q3–Q5 open, render and fold on desktop (1440px) and mobile (390px), with no JS
errors or horizontal page overflow. Mesh, model curves and timing plots visually
inspected; all results stay under `studies/results/theory-{3,4,5}/`.

### Shared completion rules

- Each study owns `studies/theory-X/`; generated evidence belongs in
  `studies/results/theory-X/<run-id>/`. Save commands, configurations, hashes,
  raw traces, exact run statuses, CSV/JSON, analysis and findings.
- Freeze designs, boundaries and acceptance rules before full execution.
  Simulation/data validity is separate from theoretical prediction accuracy.
- Theory first, controls second, measured evidence and answer next. Fit only
  genuinely unknown coefficients; do not invent a fit for parameter-free theory.
- Missing counters stay missing, not zero. Resource-stall sums are not additive
  elapsed time. Keep finite-batch and sustained-load claims distinct.
- Do not discard inconvenient cases, redefine demand after backpressure, use
  target-run outcomes as prediction inputs, or show planned data as measurements.
- Keep one self-contained question module, readable axes/units, complete folding,
  width-fitting diagrams and the white scientific dashboard style.

## Theory 2 — Maximum channel load (2026-09-09)

| Task | Status | Acceptance |
|---|---|---|
| Register sustained-throughput matrix and predictions | COMPLETE | 47 registered cases, equal eight-hop XY routes; six design tests pass |
| Implement real-NIC traffic source, trace measurement and runner | COMPLETE | Study-local paced source, real NIC/router; source backlog and total outstanding data measured separately; catalog entry works |
| Validate design and focused simulation preflight | COMPLETE | `1788972338603009434`: 4/4 validated, sources/binaries unchanged; six design tests pass; shared overload reaches link capacity but unequal source rates are retained |
| Execute full sweep and publish findings | COMPLETE | `1788972596855947793`: 47/47 valid, unchanged sources/binaries, 10/10 long-window comparisons pass; saturation rates match; eight overload equal-share disagreements and one half-window stability flag retained in saved findings |

## Theory 1 — Uncontended latency (2026-09-09)

| Task | Status | Acceptance |
|---|---|---|
| Register two-arm design and holdouts in `studies/theory-1` | COMPLETE | 336 cases/arm, 30 reference calibration cases; fixed 33×2 geometry and timestamp conventions |
| Build real-router continuous source and integrated SPM guest | COMPLETE | 14/14 preflight cases validate in `1788938725060799024`; no hardware timing changes |
| Validate boundaries, gaps, bytes and prediction | COMPLETE | `1788938825715989498`: 672/672 valid, sources/binaries unchanged; reference 306/306 exact holdouts; integrated model rejected (236/306 outside tolerance), residual equals observed 14–16-cycle source gaps |
| Finish study runner, records, analysis and documentation | COMPLETE | `studies/results/theory-1/<run-id>` CSV/JSON/report/provenance; five study and two catalog tests pass; `studies/run.py theory-1` discovers entry |

## Q7 endpoint versus link waiting (2026-09-09)

| Task | Status | Acceptance |
|---|---|---|
| Register matched geometry, sweeps and prediction contract | COMPLETE | `endpoints/README.md`: 336 cases, 72 training/120 held-out primary cases, 144 extra solo controls |
| Implement and validate endpoint/packet/SPM observations | COMPLETE | Fixed analyzer RX IDs 5–8 and streaming-before-full-frame ordering; full payload/route/byte/port validation; 4 Q7 unit tests and 12 envelope tests |
| Run fresh controls and fit endpoint cost equation | COMPLETE — FITS REJECTED | `1788935180406281131/endpoints`: 336/336 validated, unchanged binaries; physical 17/120 and guest 92/120 holdout failures retained |
| Publish Q7 interactive evidence | COMPLETE | Mesh, resource timeline, relative-cost sweeps, heatmap, fitted coefficients/residuals and causal counterexamples; desktop/mobile browser checks, no JS errors or horizontal overflow |

## Q6 packet service order (2026-09-09)

| Task | Status | Acceptance |
|---|---|---|
| Register size/order experiment and held-out split | COMPLETE | `service-order/README.md`: 259 grid and 60 held-out layouts; 957 matched runs |
| Build guest, validate traces, calibrate solo injection | COMPLETE | Preflight `1788932784234489787/service-order`: exact payloads/routes, matched binaries; zero completion/packet prediction error; 4 local + 3 existing predictor tests pass |
| Run sweeps and score packet-policy predictions | COMPLETE | `1788932921554588878/service-order`: 957/957 validated; zero held-out message/batch/packet timing or order errors; no refitting |
| Publish Q6 evidence and interactive report | COMPLETE | Measured mesh, packet timeline, slowdown sweep/heatmap, batch and prediction comparisons; desktop/mobile controls and no-overflow checks pass |

## Source-root organization (2026-09-08)

- COMPLETE: Move build/test orchestration to `tools/hardware/`, with its unit
  tests in `tools/hardware/tests/`.
- COMPLETE: Preserve refactoring reports under `docs/hardware/history/` and
  keep current invariants and this queue under `docs/hardware/`.
- COMPLETE: Replace the source-root historical guide with a concise source map;
  preserve component correctness tests in `src/sst/tests/`.
- COMPLETE: Public SST build, host checks, study catalog tests and all 33 hardware
  groups pass. Hardware evidence:
  `build/src/test-runs/1788846091479858798/results.json`.
  Migration evidence and logs: `build/src/source-layout-cleanup/1788845882499219096/`.
  All 149 checked implementation/configuration files retain their original hashes.

## Communication envelope (2026-09-08)

Implemented hardware-only studies and measurement work, and repaired both
exposed accounting/ordering failures: see
[E0–E8 queue](../../studies/compute-communication/communication-envelope/TASK_QUEUE.md).
This adds evidence and controls for RX attribution, concurrent clients,
many-to-many communication, distance/contention, sustained descriptors, SPM
banks, and held-out cost prediction. Architectural resources stay unchanged.
The final matrix passes all 54 cases. The final hardware gate passes all 33
groups, including exact CPU boundary accounting and NIC deferred-credit wakeup
regressions: `build/src/test-runs/1788830211294033294/results.json`.
The held-out cost fit remains rejected, not retuned or presented as validated
compiler guidance. Historical failures remain in the
[repair evidence](../../studies/compute-communication/communication-envelope/REPAIRS.md).

## Targeted simplification (2026-09-07)

Seven approved changes, in order. Preserve scheduling, timing, counter semantics,
and callback lifetime rules. No compiler/model runs or architectural changes.
Pinned baseline: `build/src/baselines/simplification-before-1788823346684696712/install`.

| ID | Status | Task | Acceptance |
|---|---|---|---|
| S1 | COMPLETE | Use canonical path selection when pinning baselines | 4 baseline tests pass; real override capture in `build/src/infrastructure-1788821292/baselines/simplification-path-check-1788823503975785751/` |
| S2 | COMPLETE | Delegate group runs to the authoritative test runner | 13 runner tests pass; group entry passes mesh-3x3: `build/src/test-runs/1788823564671476499/results.json` |
| S3 | COMPLETE | Remove unreachable SST build branch | Shell syntax passes; public builder passes: `build/src/build-sst-1788823590261376107.json` |
| S4 | COMPLETE | Share TX DMA beat bookkeeping | Eight fixed T1/T2/T4 fixtures pass on both binaries; zero counter/ordered-observation differences: `build/src/regressions/1788823695987664467/comparison.json` |
| S5 | COMPLETE | Share RX frame-claim validation | Software payload/order/head-blocking and RAM/SPM deployment fixtures pass; zero differences: `build/src/regressions/1788823778410864405/comparison.json` |
| S6 | COMPLETE | Extract Tile progress diagnostics | Standalone field/format golden passes; CPU timing + eight TX fixtures match, including final progress fields: `build/src/regressions/1788824036825369641/comparison.json` |
| S7 | COMPLETE | Share lease-guarded capture callbacks | Replay + nine capture modes pass under ASan/UBSan; 18 frozen-guest simulations per binary match all 132 summaries/UART logs per arm: `build/src/simplification-capture-comparison.json` |
| SV | BLOCKED | Final fresh-guest full-suite acceptance | Concurrent edits in the shared Sculptor runtime prevent guest compilation; both 31-case arms failed their runtime build. Those edits were preserved; rerun when the dependency is buildable. |

S1–S7 implementation and focused verification are complete. Full fresh-guest
acceptance is NOT complete. The failed attempts and passing scoped comparisons
are recorded in [SIMPLIFICATION_ACCEPTANCE.md](history/simplification-acceptance.md).
Existing component tests stay beside their components. Git index unchanged.

## Infrastructure repair (2026-09-07)

The active implementation is now `src/`. The R0–R11 section below is historical
acceptance evidence, not instructions to recreate the removed comparison trees.
This pass changes build/test/reporting infrastructure only: no hardware timing,
new counters, controller cleanup, compiler/model runs, dependency rebuilds or Git
index/commit changes. Shared runtime sources remain a hardware guest dependency.

| ID | Status | Task | Acceptance |
|---|---|---|---|
| I0 | COMPLETE | Preserve source and binary baseline | `build/src/baselines/infrastructure-before-1788821292692776933/manifest.json`; matched existing build provenance; initial Git index SHA256 `50f81be01c2dea35cc13f4c556d852836ff9f9fd7e803e912ffe73494b1121cc` |
| I1 | COMPLETE | Complete source/build-path migration | Fresh build `build/src/infrastructure-1788821292/build-all-1788821456999479176.json`; default-script build `build/src/build-all-1788821903973310893.json`; 10 path/ownership tests pass; GNU Make dry-run selects reorganized platform |
| I2 | COMPLETE | Standalone verification and explicit comparison | Host verification passes without reference sources; 7 comparison tests pass; paired 29-group comparison has zero counter/ordered-observation differences |
| I3 | COMPLETE | Authoritative hardware test runner | 10 runner tests pass; public hardware command passes all 29 groups; per-case outputs isolated |
| I4 | COMPLETE | Live measurement validation and final acceptance | 390 actual summaries valid; paired 29-group comparison has zero differences; default-install smoke passes; see `INFRASTRUCTURE_ACCEPTANCE.md` |

Additional migrated-entry checks: global-DMA clock oracle (13 cases) and CPU
memory-deadline oracle (12 cases) pass across 500MHz, 1GHz and 2GHz using the
fresh hardware installation. Outputs are under the acceptance build's
`oracles-global/` and `oracles-cpu/` directories. No study sweeps were run.
Final source/build/test changes remain uncommitted and the Git index SHA256 is
unchanged. Detailed scope, commands and retained artifacts are recorded in
[INFRASTRUCTURE_ACCEPTANCE.md](history/infrastructure-acceptance.md).

### Historical refactoring record

Objective: make the simulator modular and explicitly parameterized while
preserving the reference machine's functional behavior and simulated timing.

This queue covers code cleanup and refactoring, not only directory moves.
Work in `src2/`; keep `src/` as the reference. Changes to shared build/test
infrastructure must preserve the reference build and explicitly select a source
tree. The existing H1–H14 study queue remains separate.

## Status and execution rules

Statuses: `QUEUED`, `ACTIVE`, `VALIDATING`, `COMPLETE`, `BLOCKED`.
Mark a task active before implementation. Record commands, artifact paths,
results, and unresolved issues before marking it complete. A blocked task must
state its dependency. Do not mark a task complete based only on files moving.

- Retain component correctness tests beside their components in `src2/sst/tests/`.
  Top-level `tests/` owns integration checks; `studies/` owns characterization.
- Use targeted hardware checks for each change; no Sculptor-MLIR compiler/model
  suite is part of this queue. Record any shared runtime build dependencies.
- Compare identical configurations, kernels, payloads, and routes. Require exact
  functional results and deterministic timing/counter agreement where defined.
  Investigate differences; never loosen expectations merely to obtain a pass.
- Separate real model fixes from structural changes and document their intended
  timing differences. Do not silently preserve a discovered correctness bug.
- Preserve original results and use isolated output directories for src2 runs.
- Remove code only with reference/build evidence that it is unused or redundant.
  Retain optional hardware paths until their removal is separately decided.

## Ordered tasks

| ID | Status | Task | Depends on | Completion evidence |
|---|---|---|---|---|
| R0 | COMPLETE | Create the structural comparison copy | — | 133 mapped files; original snapshot unchanged; four copied host component tests passed |
| R1 | COMPLETE | Isolate src2 builds, installs, and test execution | R0 | QEMU/SST and guest build paths select src2; artifact provenance proves which binaries run |
| R2 | COMPLETE | Establish the comparison baseline and regression matrix | R1 | Saved reference/src2 functional, timing, and counter comparison with explicit coverage and exclusions |
| R3 | COMPLETE | Centralize architecture configuration and validation | R2 | Resolved machine description with matching defaults and supported-combination checks |
| R4 | COMPLETE | Centralize memory address decoding and range validation | R3 | CPU/TX/RX/DMA use common rules; boundary and invalid-range tests pass |
| R5 | COMPLETE | Make timing units and conversion boundaries explicit | R3 | Defined tick/cycle types and rounding rules; clock and accounting regressions pass |
| R6 | COMPLETE | Extract RX controller and its state from Tile | R4, R5 | Ordinary-RAM and SPM RX, completion ordering, and backpressure retain baseline behavior |
| R7 | COMPLETE | Extract TX controller and lane scheduling from Tile | R4, R5, R6 | T1/T2/T4, shared-link limits, FIFO backpressure, and SPM arbitration retain baseline behavior |
| R5a | COMPLETE | Correct global-DMA clock boundary if reproduced | R5 | Local-SPM-dominated waits cannot retire before their CPU-domain deadline; explicit before/after evidence |
| R8 | COMPLETE | Extract CPU execution/replay accounting from Tile | R5, R5a, R7 | Scalar/RVV counts and timing remain invariant across replay and batching boundaries |
| R5b | COMPLETE | Verify and guard CPU delivery and SPM retry deadlines | R8 | An unrelated store response cannot process a future CPU event or complete unfinished SPM service |
| R9 | COMPLETE | Reduce Tile to integration and lifecycle responsibilities | R6, R7, R8, R5b | Subsystems own their queues/state; initialization, barriers, analog, and teardown regressions pass |
| R10 | COMPLETE | Standardize measurement ownership and semantics | R9 | Named read-only snapshots, compatible CSV and versioned JSON; 712 actual summaries and 27 validator tests pass |
| R11 | COMPLETE | Finish cleanup and validate parameterized model | R10 | Scoped removals and formatting; 26 full + 7 focused groups match the corrected checkpoint; see ACCEPTANCE.md |

Execution order: R1 → R2 → R3 → R4 → R5 → R6 → R7 → R5a → R8 → R5b → R9 → R10 → R11.
R5a's isolated reproduction can run while R7 is edited; changes to the shared
Tile implementation wait for R7's comparison to finish.
Each extraction includes its own duplication, naming, and stale-comment cleanup.
Independent R10 writer/schema preparation can proceed while R5b/R9 own Tile;
its Tile integration and completion gate still follow R9.
R9 edits may proceed against the pinned R5b candidate while that installed binary
is tested. No R9 library installation or acceptance precedes the R5b handoff.
Disjoint R11 configuration/network cleanup and documentation can proceed during
R9/R10 edits; final acceptance still follows their integration and shared gates.

## Task details

### R1 — Independent build and test selection

- Add explicit selection of source, prepared-source, build, and install roots.
- Update QEMU overlay preparation, SST registration/build lists, platform source
  selection, and component/integration runners for the reorganized paths.
- Build into separate locations; do not overwrite reference installed libraries.
- Verify loaded SST element paths and launched QEMU/guest binary identities.
- Update `verify.py`: retain the original snapshot check, but replace its
  include-only equivalence restriction as intentional refactoring begins.

### R2 — Baseline and checks

- Capture source revisions/dirty-state fingerprints, toolchain identities,
  resolved parameters, commands, logs, and result manifests.
- Cover CPU/RVV accounting, memory/DMA readiness, routing, RX destination
  selection, TX concurrency, analog execution, barriers, and teardown.
- Include SPM RX/TX overlap and contention checks using focused fixtures from
  existing studies; preserve historical study results.
- Separate execution validity, measurement validity, and performance regression.
  Record missing dependencies and flaky log checks explicitly.

### R3 — Architecture configuration

- Inventory existing parameters and distinguish hardware resources from host
  execution/batching controls, profiling options, and workload choices.
- Define one validated description and adapters for SST, QEMU, and guest support.
  Emit resolved configuration with every run; reject incompatible combinations.
- Represent CPU/RVV, SPM capacity/banking/ports/width/latency, TX lanes/FIFOs,
  existing RX queues/bandwidth, NIC injection/ejection, routers/links, global RAM,
  and analog resources. Preserve current defaults and supported behavior.
- Do not invent RX2/RX4 capability or change architectural defaults in this task.

### R4 — Memory domains

- Centralize base addresses, full-range membership, bounds, alignment requirements,
  and overflow-safe validation. Replace duplicated address subtraction/checks.
- Exercise ordinary RAM with SPM disabled and enabled, SPM endpoints, crossing
  boundaries, invalid ranges, and zero-length behavior as defined by each ABI.
- Keep every SPM client on the common bank/port arbitration path.

### R5 — Timing boundaries

- Distinguish SST ticks, CPU cycles, DMA cycles, and other device clock domains.
- Centralize conversion/rounding and overflow handling; never complete early.
- Distinguish elapsed intervals from accumulated service and stall counts.
- Test nonidentical supported clocks and retain replay-accounting invariants.

### R6 — RX controller

- Create `network/rx/rxController.{h,cc}` with ownership of descriptors, frame
  assembly, receive queues, in-flight DMA, invalidation tracking, and completion.
- Inject memory/network/event interfaces instead of accessing Tile internals.
- Keep shared SPM arbitration, ordinary-memory RX timing, ordered completion,
  descriptor setup accounting, bounded queues, and upstream backpressure.

### R7 — TX controller

- Create `network/tx/txController.{h,cc}` owning descriptors, lane state, FIFO
  occupancy, scheduling, completion, and serialization/backpressure tracking.
- TX lanes compete for existing banks, NIC capacity, and physical links.
- Preserve bounded streaming and functional payload-snapshot semantics.
- Test one/two/four lanes, independent directions, same-link contention, and
  shared SPM demand; do not create per-lane copies of physical bandwidth.

### R8 — CPU execution and replay

- Move grant/yield, replay segment baselines, instruction accounting, and pending
  CPU events into explicit execution components with clear state transitions.
- Keep instruction transport/batching separate from modeled issue timing.
- Check final segments, vector memory boundaries, scalar/RVV mixes, and epoch
  transitions. Preserve the complete-region charged-cycle invariant.

### R9 — Tile coordinator

- Move remaining initialization, DMA orchestration, and analog coordination state
  to their owners where appropriate. Define interfaces for wakeups and readiness.
- Tile retains SST lifecycle, wiring, and event dispatch; controllers expose
  progress/completion interfaces rather than allowing external queue mutation.
- Check callback lifetimes, cancellation, drain conditions, guest failure, and
  clean shutdown as well as successful execution.

### R10 — Measurement

- Record counters at actual resource decisions with documented measurement scope.
- Represent unavailable counters explicitly; do not substitute zero silently.
- Define request/byte conservation, queue and lane histograms, completion windows,
  and per-client service metrics. Preserve compatibility or version schemas.
- Ensure analysis and dashboards identify the configuration and implementation.

### R11 — Cleanup and acceptance

- Audit remaining duplicated logic, misleading names, stale comments, unused
  fields/functions, and obsolete branches; record evidence for removals.
- Update module responsibilities, dependencies, build commands, parameter
  definitions, supported configurations, and the path/comparison documentation.
- Run the selected hardware regression matrix and focused parameter comparisons.
- Deliver before/after results and unresolved limitations. Keep the production
  switch and new architecture capabilities as explicit subsequent decisions.

## Evidence log

- R10/R11 complete: [ACCEPTANCE.md](history/source-refactor-acceptance.md) records the exact final build,
  pinned binaries, commands, comparison scope and remaining measurement limits.
  All 26 full groups (52 arms) and seven focused groups (14 arms) pass with zero
  counter/ordered-observation differences from the corrected R9 checkpoint.
  All 28 configuration checks, 12 host groups, owner sanitizers, 13 global-DMA
  oracles, 24 CPU/SPM oracle arms and serial/parallel capture identities pass.
  All 712 final JSON/CSV summaries and 27 validator tests pass. The original
  133-file snapshot remains unchanged; default source selection is still src.
  Final checkpoint: `build/src2/baselines/refactor-complete-1788566243867779900/install`.

- R9 complete: new physical memory, analog, global-DMA and initialization/barrier
  owners retain one shared SPM. CPU capture stops before borrowed resources die;
  the global-RAM descriptor is RAII-owned. Full 26 groups and focused timing
  gates pass with zero differences against the corrected checkpoint. All clock,
  deadline, ready-set and new owner/lifetime tests pass, including sanitizers.
  Evidence: `sst/execution/R9.md`, `build/src2/r9-handoff-pPxiZN/manifest.json`.
  Pinned before final measurement integration:
  `build/src2/baselines/r9-1788565130287235401/install`.
  This validated build also contains the disjoint R11 cleanup and the R10 writer
  foundation. All 28 configuration checks pass at
  `/tmp/golem-src2-configuration-1_jy57d6`.
- R5b accepted: all 12 corrected deadline cases pass, all 13 R5a clock cases
  remain correct, and all 26 hardware groups have functional pass evidence
  (component rerun after host-writer link repair). The 20 summary/12 observation
  differences in two DMA groups are explicitly reviewed timing corrections,
  not zero differences. Details and complete evidence: `sst/execution/R5b.md`.
  Accepted hardware: `build/src2/baselines/r5b-candidate-1788563863788379039/install`.
- R5b reproduction: `build/src2/cpu-memory-deadline-lvo6xiim/results.json`
  records the pinned R8 binary failing both buffered-store timing oracles at
  500 MHz, 1 GHz and 2 GHz; single-entry blocking-store controls and functional
  payload checks pass. The fixture uses cacheless StandardMem, not L1/L2.
  At 1 GHz the register-only region ends at tick 156000 although its instruction
  lower bound is tick 4143000. The SPM case also retires unfinished service.
  These are intentional correctness repairs, not equivalence expectations.
- R10 preparation: isolated Mittens builds now embed a deterministic source-input
  identity and refuse installation if implementation inputs change during the
  build. Source-selection/input-fingerprint host checks: 7 PASS. The versioned
  measurement writer is being prepared separately; no new measurements or
  completed Tile integration are claimed yet.
- R5a: complete, intentional correctness change. The integrated local-SPM
  deadline oracle reproduced early/incorrect retirement in both pre-fix trees:
  `build/src2/global-dma-clocks-10udkqcn/results.json`. Corrected cycle-to-tick
  deadline construction, tick-to-cycle wait rounding, and unrelated completion
  wakes bypassing an armed deadline. All 13 src2 cases now pass at
  `build/src2/global-dma-clocks-pngu71uq/results.json`; all 13 original-source
  cases still fail the timing oracle, with functional payload checks passing.
  Existing three memory groups retain exact counters/reports at
  `build/src2/regressions/1788560891741879931/comparison.json`.
  Corrected build: `build/src2/build-sst-1788560882726524923.json`.
  Pre-R8 pinned hardware/source fingerprint:
  `build/src2/baselines/r7-r5a-1788560931812394855/manifest.json`.
  Full 26-group original-reference matrix also passed both trees with zero
  summary and hardware-observation differences at
  `build/src2/regressions/1788560984103894415/comparison.json`.
- R7: complete; `network/tx/txController.{h,cc}` owns descriptors, per-lane
  FIFO/frame state, timed availability and TX counters. Tile retains links,
  guest wake order and lifecycle decisions. The original five targeted groups
  passed at `build/src2/regressions/1788560421277129261/comparison.json`.
  H2 direction sharing, H6 bank layouts and H7 FIFO depths all passed at
  `build/src2/regressions/1788560592713409394/comparison.json`. Both comparisons
  have zero summary-counter and deterministic hardware-observation differences.
  The latter also compares all lane opportunity reports and router statistics.
- R6: complete; `network/rx/rxController.{h,cc}` owns frame assembly, claims,
  descriptors, timed receive queues, invalidation tracking and RX counters.
  Tile supplies shared resources and typed event scheduling, and retains CPU
  wake/lifecycle ordering. Build: `build/src2/build-sst-1788559790657191057.json`.
  Five targeted groups passed on both trees with zero counter differences:
  `build/src2/regressions/1788560019977414812/comparison.json`.
  Six host tests and all 23 configuration checks passed. Intentional diagnostic
  change: invalid RX width now fails centralized configuration validation before
  constructing the RX engine; valid configuration timing is unchanged.
  Earlier inherited-environment failures are retained. The regression runner
  now discards inherited study fixture knobs and exits nonzero on discrepancies.
- R0: `path-map.json`; `python3 src2/verify.py` passed for RX DMA, scratchpad
  timing, memory-access coalescing, and global-RAM readiness. At that structural
  copy stage, src2 QEMU/SST integration had not yet been built or validated.
- R1: complete; `python3 src2/build.py sst -j 8`,
  `python3 src2/build.py qemu -j 8`, and `python3 src2/check-build.py` passed.
  Manifests: `build/src2/build-sst-1788557636714847133.json`,
  `build/src2/build-qemu-1788557580769922557.json`;
  integrated exec/load/DWARF proof: `build/src2/checks/1788557615098996007/`.
  Invalid reference output-root selection was rejected. Original snapshot and
  four host component checks passed. QEMU overlay include paths were corrected.
  Existing locally modified SST Core is a shared, read-only dependency; no
  element installation may change its registry during the comparison.
- R2: complete; `python3 src2/regression.py` saved both hardware runs at
  `build/src2/regressions/1788557787554685560/comparison.json` with zero summary
  counter differences. The src2 epoch-barrier assertion failed on interleaved
  UART text, not barrier behavior. Its test now uses per-tile serial files;
  both reruns passed at `build/src2/regressions/1788557949794577074/`.
  Focused TX-lane and modeled-TX/SPM cases passed on both trees with zero
  counter differences at `build/src2/regressions/1788557823387261664/`.
  Original failures remain preserved, not overwritten or relabeled.
- R3: complete; typed tile/router/NIC/global-RAM configuration and shared JSON
  snapshots are in `sst/configuration/`. All 23 configuration checks passed
  (`/tmp/golem-src2-configuration-nfopv8us`). The full 26-case hardware matrix
  passed on both trees with zero summary differences at
  `build/src2/regressions/1788558599189695661/comparison.json`; focused TX/SPM
  checks also matched at `build/src2/regressions/1788558597120576454/`.
  An intermediate extraction omitted port metadata; this was restored and the
  failed run remains at `build/src2/regressions/1788558212140111925/`.
- R4: complete; C/C++ `MemoryMap.h` and SST `AddressRegion` share base and
  range rules across QEMU, guest headers, CPU, TX, RX and global DMA. Host
  boundary/overflow/exhaustive-small-range tests and C11 header compilation
  passed. CPU namespaced memory now also rejects a span crossing its upper
  boundary (previously only the starting address was checked); valid timing
  remains unchanged. Six selected hardware/overlap gates passed on both trees
  with zero differences at `build/src2/regressions/1788559007396237608/`.
  Integrated build provenance passed at `build/src2/checks/1788559008565404330/`.
- R5: complete; `execution/clockDomain.h` gives distinct cycle-domain types,
  checked tick arithmetic and explicit floor/ceil conversion. Host exhaustive
  rounding/overflow checks passed. Real 300-word ordinary-RAM RX takes 92 CPU
  cycles at CPU=1GHz/RX=500MHz and CPU=2GHz/RX=1GHz, and 23 CPU cycles at
  CPU=500MHz/RX=1GHz, with SPM both enabled and disabled. Both implementations
  passed these and the CPU/RVV, SPM, analog and overlap gates with zero counter
  differences: `build/src2/regressions/1788559279054526024/comparison.json`.
- R8: complete. Final full 26-group comparison and three focused groups pass
  with zero counter and ordered-observation differences:
  `build/src2/regressions/1788562505027570205/comparison.json` and
  `build/src2/regressions/1788562506219123663/comparison.json`.
  All 13 global-DMA clock oracles, nine ready-set identities, host ledger and
  controller tests, and sanitizer checks pass. Detailed ownership and evidence:
  `sst/execution/R8.md`, `build/src2/r8-handoff-Yrn8we/manifest.json`.
  Pre-R5b pinned hardware: `build/src2/baselines/r8-1788563110972463702/install`.
  The extraction separated the pure CPU ledger, per-tile execution/replay state,
  and shared capture coordination against the pinned post-R5a hardware baseline.
  Pre-extraction ready-set gate passed all nine serial/parallel comparisons
  (18 simulations): `build/src2/ready-set-baseline-zetg5w/tests/qemu-ready-set-sst.BDFEEl/`.
  The pinned-binary runner itself matched CPU timing exactly at
  `build/src2/regressions/1788561229364368776/comparison.json`.
- Initial R9–R11 audit note (superseded by completion evidence above): read-only
  ownership/measurement/cleanup audits informed the subsequent implementation.
- Initial R5b audit note (subsequently reproduced and fixed): an ordinary buffered
  store response can call pending CPU processing before a captured instruction
  interval is due. The direct-SPM service latch also lacks an explicit deadline.
  Reproduce with a cacheless/fake timing endpoint; do not reintroduce L1/L2 tests.
  Separate this inherited correctness issue from R8 structural equivalence.
