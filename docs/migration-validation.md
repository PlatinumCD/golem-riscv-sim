# Component release validation

Validated on 2026-09-29 in an isolated release worktree. All **23 maintained
suites passed**, including the profiling regression. The initial full run had
one environment failure: the worktree lacked its link to the installed GNU
RISC-V compiler. After restoring that dependency, the affected CPU suite passed
on retry. The original logs retain the failure and the separate successful retry.

## Build and regression commands

Normal reproduction with the shared dependencies installed:

```sh
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite all
bash tests/run-all.sh --case platform/llvm-rvv --profile
```

This validation freshly built the production SST libraries and the library with
correctness fixtures. It reused the installed SST, LLVM, GNU and Sculptor
compilers and the existing QEMU executable. All 38 recorded QEMU source inputs
match the release source; QEMU itself was unchanged. This was not a full rebuild
of LLVM, QEMU or SST core. Mordred's vendored files match their pinned hashes;
observation hooks are applied only to build-local copies.

## Coverage

| Area | Suites | Result and coverage |
| --- | ---: | --- |
| Host tooling and configuration | 1 | PASS; 80 checks and generated parameter-reference consistency |
| Component composition | 1 | PASS; scratchpad and array fixtures |
| CPU and RVV | 6 | PASS; QEMU synchronization, instruction cache, vector memory, LSQ, compressed scalar instructions, and 16 freshly compiled LLVM kernels |
| Optional profiling | 1 | PASS; unset/off/on controls, identical timing and values, physical flit timing and instruction-budget invariance |
| SPM | 2 | PASS; range ordering and shared CPU/router bank connections |
| Analog accelerator | 7 | PASS; vector transfers, numerical outputs, pipeline, register dependencies, command queue/admission/backend, and 65 programming-delay cases with 16 paired controls |
| Network | 4 | PASS; 2×2 Mordred mesh, complete tiles attached through SPM, local SPM service and posted transfers |
| Sculptor compiler integration | 1 | PASS; eight single-tile cases at VLEN 256 and 512, including multi-array linear and convolution workloads |
| **Total** | **23** | **PASS after resolving the compiler-path setup failure** |

The LLVM fixture is self-contained: its C kernels, linker script, trace analysis
and validators are maintained under `src/tests/llvm-rvv`. The Sculptor fixture
includes its required local runtime and ABI headers. Neither needs a study
directory, saved experiment binaries or dashboard assets.

## Profiling checks

The four-tile regression checks 40 component/port profiles. Default-off and
explicit-off runs create no profile directory even when a destination path is
supplied. Enabled and disabled runs produce identical numerical values,
architectural timing, memory images and existing event CSVs. Changing the QEMU
instruction grant from 256 to 1 preserves architectural timing and traffic.
Every recorded flit hop has the configured one-cycle link delay; no port sends
more than one flit per cycle.

The public test-runner `--profile` flag also passes all 16 LLVM cases and produces
the expected SPM profiles for each. Separately, the installed production
libraries execute the numerical vector/analog guest through
`tools/hardware/env.sh --profile`, without test-output environment variables.
`--no-profile` overrides an inherited enabled flag, creates no CSVs and gives
identical timing and memory contents: 63,774 simulated cycles in both runs.

## Local evidence and scope

Evidence remains in ignored directories; generated results are not part of the
release:

- Full suite: `tests/results/release-validation/results.json`
- Final combined validation: `tests/results/release-final-validation.json`
- CPU retry: `tests/results/release-validation-riscv-retry/results.json`
- Final host checks: `tests/results/release-final-host/results.json`
- Public profiling flag: `tests/results/release-llvm-profile/results.json`
- Production flag checks: `tests/results/release-installed-profile/validation.json`
  and `tests/results/release-installed-no-profile/validation.json`
- Production build: `build/src/current-build.json`
- Fixture build: `build/release-components/build.json`

Per-run reports identify source inputs, commands and logs. Source inputs stayed
unchanged during the full regression and its retry. After final formatting of
the SPM observation generator, both libraries were rebuilt and checked against
the tested artifact hashes. The runner also records the local compiler runtime
inputs. These checks preserve the distinction between tested artifacts and
local dependency installations.

The branch contains the component model, maintained tests and required build
support/documentation. No studies, study results or dashboard visualization
changes are included. No performance sweep was rerun. Guest-initiated network
sends and compiled multi-tile execution remain the separate integration work
described in the [migration record](migration.md).
