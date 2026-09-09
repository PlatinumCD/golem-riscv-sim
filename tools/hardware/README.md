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

SST component tests remain in `src/sst/tests/`; `verify.py` invokes them there.
Only tooling-owned Python tests moved into this directory. `build/src` and
`install/src` remain the default output roots. Historical run manifests retain
their original command paths and are not rewritten.
