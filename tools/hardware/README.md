# Hardware tooling

Host-side orchestration lives here, separate from the simulator implementation
in `src/`. The public entry points remain `bootstrap.sh`, `build-scripts/` and
`tests/run-all.sh`; callers do not need to import these modules directly.

| Files | Responsibility |
|---|---|
| `build.py`, `hardware_paths.py`, `env.sh` | Build coordination, safe paths and selected hardware environment |
| `hardware_runner.py`, `hardware_suite.py` | Explicit hardware registry, execution and retained evidence |
| `verify.py`, `check-build.py` | Host component checks and integrated binary provenance |
| `pin-baseline.py`, `regression.py`, `comparison.py` | Explicit baseline capture and comparison |
| `tests/` | Correctness tests for this tooling |

```bash
python3 -B tools/hardware/verify.py
bash tools/hardware/env.sh COMMAND
python3 tools/hardware/pin-baseline.py LABEL
python3 tools/hardware/regression.py --reference-install /absolute/baseline/install
```

Run these commands from the repository root. `COMMAND`, `LABEL`, and the
reference-install path are placeholders. Baseline capture writes a snapshot;
it is not part of ordinary verification.

`verify.py` compiles selected host component tests from `src/sst/tests/` and
runs the tooling tests. It does not run a full SST simulation suite.
Hardware builds default to `build/src/` and `install/src/`.
