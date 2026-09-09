# Infrastructure repair acceptance — 2026-09-07

Changes 1–4 are complete. This is infrastructure acceptance, not a new hardware
performance claim or acceptance of compiler/model workloads.

## Outcomes

| Gate | Result |
|---|---|
| Fresh QEMU/Mittens build, independent prefixes | PASS |
| Default `bootstrap.sh build-hardware`, fresh-shell environment | PASS |
| Public hardware suite on fresh binaries | 29/29 groups PASS |
| Actual generated JSON/CSV measurement records | 390 valid, 0 invalid |
| Explicit before/after hardware comparison | 29/29 groups PASS per arm; 0 counter differences; 0 ordered-observation differences |
| Default-install smoke | Boot and register-only scalar/RVV PASS |
| Global-DMA clock oracle | 13/13 PASS |
| CPU memory-deadline oracle | 12/12 PASS |
| Path/ownership tests | 10 PASS |
| Comparison unit tests | 7 PASS |
| Runner failure/selection/measurement tests | 10 PASS |
| Measurement-validator unit tests | 27 PASS |
| Shell syntax and Git whitespace checks | PASS |

The 390 records are 389 case summaries plus one provenance-fixture summary.
Negative unit fixtures deliberately exercise failed execution, missing outputs,
timeouts and dependency failures; their expected failures are not hardware
regressions. Component C++ tests and ownership tests also run in the host and
component groups; the group count is not an individual assertion count.

## Current public commands

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
bash tests/run-all.sh --list
bash tests/run-all.sh --case network/mesh-3x3
python3 -B src/verify.py
```

No wrapper is necessary. Build/install defaults are `build/src` and `install/src`.
Explicit output overrides are validated, including symlink ownership. Shared
SST Core, LLVM, Merlin, memHierarchy and CrossSim are reused; attempts to build
dependencies through shared-install symlinks are rejected before compilation.
The existing GNU RISC-V toolchain remains available to the installed LLVM stack.

The default bootstrap action builds/tests hardware. Compiler, model and complete
repository test selections are explicit opt-ins. No full dependency-stack or
compiler build is covered by this acceptance.

## Retained evidence

All paths below are relative to the repository root.

- Pre-change verified binary/source manifest:
  `build/src/baselines/infrastructure-before-1788821292692776933/manifest.json`
- Fresh hardware build:
  `build/src/infrastructure-1788821292/build-all-1788821456999479176.json`
- Default-script hardware build:
  `build/src/build-all-1788821903973310893.json`
- Before/after comparison:
  `build/src/infrastructure-1788821292/regressions/1788821631554963391/comparison.json`
- Final public-command run:
  `build/src/infrastructure-1788821292/test-runs/1788821879938287654/results.json`
- Default-install smoke:
  `build/src/test-runs/1788821951661858885/results.json`
- Global-DMA oracle:
  `build/src/infrastructure-1788821292/oracles-global/global-dma-clocks-5zf5l_oq/results.json`
- CPU deadline oracle:
  `build/src/infrastructure-1788821292/oracles-cpu/cpu-memory-deadline-y74awont/results.json`

The fresh hardware installation is `install/src/infrastructure-1788821292`.
Its public-suite command was:

```bash
GOLEM_BUILD_ROOT="$PWD/build/src/infrastructure-1788821292" \
GOLEM_INSTALL_ROOT="$PWD/install/src/infrastructure-1788821292" \
bash tests/run-all.sh --suite hardware
```

For the paired comparison, the same environment selected the candidate and
`python3 -B src/regression.py --reference-install "$PWD/build/src/baselines/infrastructure-before-1788821292692776933/install"`
selected the pinned baseline. Both arms used current guest/component sources.
Historical reference JSON is explicitly not validated against the current
contract; current candidate JSON is strictly validated. CSV counter rows,
duplicates and ordered hardware observations are compared without relaxation.

Current Mittens implementation ID:
`sha256:938d2e6bd308ebed4bd6c9982b2b60003f1052b96517c133f97cf0b829d82957`.
Loaded-library, QEMU and guest-image evidence is linked from each results file.
The runner rejects a current candidate whose implementation ID differs from
current source inputs. Binary and checked shared-dependency hashes reconcile
before/after execution.

## Scope and preservation

- No bank, port, lane, routing, CPU timing, DMA timing or architectural-default changes.
- The only production SST C++ change is the measurement contract path correction;
  legacy counter values and JSON schema version remain unchanged.
- No new counters, controller refactor, study sweep, Sculptor compiler execution,
  model workload, dependency rebuild or submodule update.
- Historical outputs remain unchanged. Baseline and acceptance artifacts are new.
- Source/build/test changes remain uncommitted; no staging or commits occurred.
  Git index SHA256 before and after:
  `50f81be01c2dea35cc13f4c556d852836ff9f9fd7e803e912ffe73494b1121cc`.
- This does not make the uncommitted checkout reproducible from Git HEAD alone.
  Commit boundaries and the broader measurement/controller improvements remain
  explicitly deferred.
