# Simulator source

> Historical snapshot: commands and paths below describe the earlier layout.
> Use [the current source guide](../../../src/README.md) for today's workflow.

`src/` is the active simulator implementation. `src2/` and `old_src/` are not
required. The sections below the current workflow retain the original
refactoring history; their saved paths and results describe those older runs.

## Current hardware workflow

With the installed SST Core, LLVM/GNU tools, Merlin, memHierarchy and CrossSim
dependencies available, run from a fresh shell:

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
```

The default `bootstrap.sh` action builds and tests hardware. The default test
suite is hardware, not compiler/model workloads. Discover or select cases with
`bash tests/run-all.sh --list` and `--case network/mesh-3x3`; use `--timeout 600`
to override the default 300-second per-case deadline. Full compiler/model tests
require explicit `--suite compiler`, `--suite models` or `--suite all`.
`bash tests/run-group.sh network --list` uses this same runner and selection
policy; group runs also accept `--case` and `--timeout`.

Source, build, install and prepared-source defaults are `src`, `build/src`,
`install/src` and `build/src/sources`. `GOLEM_BUILD_ROOT`, `GOLEM_INSTALL_ROOT`
and `GOLEM_SOURCE_ROOT` override generated output locations. Builds reuse shared
dependencies in `install/`; they never silently rebuild them. `src/env.sh` is
an optional convenience wrapper, not a prerequisite for the documented commands.

Host-only verification is `python3 -B src/verify.py`. Integrated runs record
loaded binary identities, per-case logs/statuses, guest hashes, configuration
references and actual JSON/CSV measurement validation in a unique test-run
directory. `NOT_APPLICABLE` means a case produces no measurements; it is not a
claim of measured zero. A required missing artifact, timeout or invalid summary
fails the gate. Runtime guest support uses existing Sculptor runtime sources,
without building or executing the Sculptor compiler.

Historical comparison is explicit:

```bash
python3 src/pin-baseline.py LABEL
python3 src/regression.py --reference-install /absolute/pinned/baseline/install
```

Baseline capture honors the same build/install overrides as builds. Comparisons
include deterministic final progress fields, excluding host wall time and
periodic sampling. Targeted simplification evidence is tracked in
[TASK_QUEUE.md](../task-queue.md).

Both arms use current test/guest sources. The reference binaries must match
their pinned manifest. Historical JSON is not relabeled or validated against a
new contract; current candidate output must validate. Comparisons preserve
legacy counter values, duplicate observations and ordered hardware traces.
Source copy-equivalence is a separate historical check requiring
`src/verify.py --check-copy-equivalence --reference-source /original/layout`.

## Historical src2 refactoring record (not current run instructions)

The cleanup and refactoring plan is tracked in [TASK_QUEUE.md](../task-queue.md).
It covers isolated builds, configuration, address/timing rules, RX/TX and CPU
extraction, measurement, and final hardware validation in dependency order.

This is the refactoring workspace; `src/` remains the reference implementation.
The default repository bootstrap still selects `src/`.

R0–R11 are complete, including the separately reproduced R5a/R5b timing
corrections. The final 26-group hardware matrix and seven focused groups match
the corrected checkpoint exactly. See [ACCEPTANCE.md](source-refactor-acceptance.md) for the
build, pinned binaries, validation evidence and limits. This does not switch
the default source tree or validate compiler/model workloads.

## Isolated build and tests

```bash
python3 src2/build.py all -j 8
python3 src2/check-build.py
bash src2/env.sh bash tests/platform/cpu-timing/run-test.sh
bash src2/env.sh bash src2/sst/tests/run-test.sh
python3 src2/verify.py
python3 src2/sst/tests/device_owners.py
python3 src2/sst/tests/test_measurements.py
```

Mittens and QEMU build from this tree into `build/src2` and `install/src2`.
SST Core, LLVM, Merlin, memHierarchy, and CrossSim are shared read-only build/run
dependencies; SST registration is not changed. Guest builds still use the shared
RISC-V runtime sources from `third_party/sculptor-mlir/runtime`, but do not build
or run that compiler/model suite. The runtime library is installed separately.

`check-build.py` checks actual library loading and QEMU execution with `strace`,
and guest DWARF paths; build manifests record inputs, commands, and artifact
hashes. Isolated Mittens builds embed a deterministic implementation-input ID
and reject installation if implementation inputs change during compilation.
That ID is distinct from the installed binary's hash. `verify.py` preserves the
reference snapshot check and host tests;
`--check-copy-equivalence` optionally enables the original include-only check.
Select a different output root with `GOLEM_BUILD_ROOT`, `GOLEM_INSTALL_ROOT`,
and `GOLEM_SOURCE_ROOT` (prepared third-party sources). Hardware source selection
is `GOLEM_HARDWARE_TREE=src|src2`; comparison builds reject reference output roots.

| Directory | Owner |
|---|---|
| `bridge/` | Shared QEMU/SST ABI definitions |
| `qemu/devices/` | QEMU synchronization, NIC, and analog devices |
| `qemu/instructions/` | Custom instruction decoding and helpers |
| `sst/tile/` | SST lifecycle, resource wiring, event dispatch and measurement integration |
| `sst/execution/` | CPU grant/replay controller and ledger, delivery deadlines, QEMU process/capture coordination, neutral CPU actions and descriptor RAII |
| `sst/configuration/` | Validated tile, router, NIC and global-RAM parameters |
| `sst/network/` | Router, NIC and packet events |
| `sst/network/rx/` | Receive protocol, descriptors, DMA completion and RX counters |
| `sst/network/tx/` | TX descriptors, lanes, bounded FIFOs, scheduling and serialization counters |
| `sst/memory/` | MemoryAccessController owns ordinary requests/store buffering and the single SPM model; GlobalDMAClient owns endpoint tokens/deadlines; GlobalRAMController owns shared RAM service/readiness |
| `sst/synchronization/` | Per-tile InitializationBarrierClient plus shared epoch and initialization barrier controllers |
| `sst/bridge/` | Host implementations of shared-memory bridges |
| `sst/analog/` | AnalogController owns submissions, deferred work, completion/wakes, device/bridge and counters; devices/backends model execution |
| `sst/profiling/` | Owned Tile measurement snapshots, compatible CSV/versioned JSON writer, trace recording and TaskTrace |
| `sst/probes/` | Synthetic SST requesters and validation probes |
| `sst/tests/` | Component checks |
| `platform/startup/` | Boot assembly, linker script, and exit |
| `platform/devices/` | Guest interfaces and UART support |
| `platform/runtime/` | Freestanding memory, math, and MLIR helpers |
| `platform/deployment/` | Compiler deployment entry points and inputs |
| `config/build/` | Toolchain and pinned dependency versions |
| `config/architectures/` | Epoch configuration snapshots |
| `patches/` | Upstream integration patches |

`path-map.json` maps every original source file to its new location and records
the original SHA-256. Generated Python caches are excluded from the source map.
The SST Automake source list uses the reorganized paths. `build.py` uses the
same list for the isolated library build. Production selection remains separate.

## Architecture

- R0–R2 established the mapped comparison copy, isolated build/install selection,
  provenance and paired hardware gates.
- R3–R5 centralized immutable validated configuration, shared address/range
  rules, and typed clock domains with explicit overflow/rounding boundaries.
- R6/R7 moved receive descriptors, frame assembly, invalidations and completion
  into RX, and transmit descriptors, lanes, bounded FIFOs and scheduling into TX.
- R8 moved grant/capture/replay state into `CpuExecutionController`, instruction
  accounting into its ledger, and shared capture coordination into its own owner.
  Initial, runtime and local lookahead paths retain revocable callback lifetimes.
- R9 extracted analog, ordinary-memory/SPM, global-DMA and initialization/barrier
  orchestration. Tile composes these owners through typed actions, narrow host
  callbacks and resource services: no whole-Tile backpointer, friend access or
  external queue mutation.
- R10 separates measurement snapshots, rendering and task-trace context from
  resource execution. R11 removes verified dead code/redundant checks and
  standardizes the extracted code's formatting and documentation.

MemoryAccess owns one SPM timing model shared by CPU, global DMA, RX and every TX
lane; extraction does not duplicate banks, ports or network bandwidth. Service
order remains RX → TX → analog. Ordinary-RAM paths and QEMU's functional payload
snapshots are preserved. CPU capture is stopped/drained before borrowed resources
are destroyed; file-descriptor RAII closes only the parent's owned duplicates.
See [R8's execution contract](../../../src/sst/execution/R8.md),
[R9's ownership/lifetime and evidence report](../../../src/sst/execution/R9.md), and
[the preserved invariants](../refactor-invariants.md).

R9's recorded full post-R5b comparison passed all 52 arms with zero counter or
ordered-observation differences:
[`1788564959235621218`](../../../build/src2/regressions/1788564959235621218/comparison.json).
Its build also includes the disjoint R11 cleanup and R10 writer foundation; it
does **not** validate the subsequent final Tile measurement integration. The
accepted R9 binary baseline is
`build/src2/baselines/r9-1788565130287235401/install`.

## Reference comparison

```bash
python3 src2/regression.py
python3 src2/regression.py --case clock/rx --case study/tx-lanes --case study/tx-spm
python3 src2/sst/tests/run-configuration-test.py
python3 src2/test-build-selection.py
python3 src2/test-comparison.py
```

The default matrix runs 26 hardware test groups on both source trees. Selected
study fixtures are opt-in; this never launches a compiler or model suite.
Each trial has a unique output directory under `build/src2/regressions/`, private
runtime installation, binary hashes, commands, logs and resolved src2 parameters.
Inherited `MITTENS_*` study knobs are discarded so each fixture controls its own
inputs. Exact comparison covers summary counters, hardware final reports and
router statistics. Unspecified component print order is ignored, but records,
duplicates and numeric values are not. Host progress time is not a timing gate.
Discrepancies, failed tests and timeouts produce a nonzero exit status and retain
all evidence. The queue links the accepted result for each completed task.

For a comparison against an accepted src2 stage after an intentional model fix:

```bash
python3 src2/pin-baseline.py accepted-stage
python3 src2/regression.py --reference-install /absolute/path/printed/above --reference-tree src2
```

Pinning copies QEMU/Mittens binaries and records source fingerprints plus build
provenance. It rejects unbuilt implementation changes. Shared dependencies and
guest/component sources are still selected separately; this is a hardware-binary
baseline, not a second complete source checkout. The legacy `src` result-folder
label denotes the reference arm; `source_tree` and `hardware_installation` in
its result record identify the actual selection.

The separate correctness oracles are:

```bash
python3 src2/sst/tests/global_dma_clock.py
python3 src2/sst/tests/cpu_memory_deadline.py --reference-install build/src2/baselines/r8-1788563110972463702/install
```

The global-DMA oracle requires correct src2 single/batch wait deadlines at three CPU clocks and
keeps the original source's known failures visible. That reference mixes CPU
cycles and SST ticks at local DMA completion; R5a fixes the units and prevents an
unrelated completion wake from bypassing a pending deadline. The oracle checks
payloads too.

R5b enforces captured CPU instruction/replay delivery and SPM-service deadlines,
defers early direct completions and rejects superseded scheduled wakes. Its
cacheless StandardMem fixture exposes buffered-store responses retiring future
instructions or unfinished SPM loads; no deleted L1/L2 tests are restored. All
12 corrected cases pass at three clocks. The six pinned-R8 overlap failures stay
visible, alongside six passing blocking-store controls. See
[the R5b oracle contract](../../../src/sst/tests/cpu_memory_deadline.md).

These intentional fixes are not refactoring equivalence. R5b's existing DMA
groups have reviewed **20 summary and 12 ordered-observation differences**:
global-DMA replies also could bypass future CPU work. Corrected comparisons must
retain those differences against pre-R5b binaries; later ownership extractions
compare against the accepted post-fix baseline, not silently relaxed expectations.

## Measurement outputs

R10 uses a named, read-only `TileMeasurementSnapshot` assembled from
owner values, plus `TaskTrace` for task records and attribution context. Resource
controllers continue to own counters; the snapshot/writer cannot mutate queues
or schedule execution. `SummarySnapshot`
feeds `MeasurementWriter`, producing legacy `tile-N-summary.csv` and
`tile-N-summary.json` with schema `mittens.summary`, version **1**.

JSON records explicit units, clock domains, aggregation/scope, timebase factors,
availability and provenance. Measured zero differs from disabled, not-measured
and unknown; unavailable JSON values are null. Legacy CSV retains compatibility
payloads and is not an availability channel. Compiled implementation identity
and source selection do not replace the run manifest's binary/toolchain hashes.
SPM service sums are not elapsed busy time; conflicts count delayed beats, not
delay cycles. Task `cpu_cycles` is guest issue cost, not elapsed region time.
Existing TX observation/coverage limitations remain explicit, not newly measured
by adopting a schema. See [the measurement contract](../../../src/sst/profiling/MEASUREMENT_CONTRACT.md).

Validate completed src2 run artifacts with:

```bash
python3 src2/sst/tests/check_measurements.py /absolute/path/to/run/artifacts
```

Optionally supply `--implementation-id sha256:...` from the build manifest to
enforce the exact expected implementation. The checker requires real Tile
provenance, the referenced configuration and neighboring legacy CSVs. It verifies
units, availability, exact integer values, clock factors and histogram sums.
Do not mix a pre-R10 reference arm into these new-schema checks.

Final acceptance checked 712 actual summaries and preserved hardware timing and
ordered traces. Implemented removals and retained capabilities are listed in
[CLEANUP_AUDIT.md](cleanup-audit.md). No compiler/model suite or wall-time
performance claim is part of this acceptance scope.
