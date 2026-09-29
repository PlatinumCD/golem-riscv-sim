# Simulator correctness tests

The maintained correctness suites live beside the components in [`src/tests`](../src/tests).
The standard runner selects the current CPU/SPM/accelerator/Mordred model.

```sh
JOBS=8 bash bootstrap.sh build-hardware
python3 -B tools/hardware/verify.py
bash tests/run-all.sh --list
bash tests/run-all.sh --suite hardware
bash tests/run-all.sh --group network
bash tests/run-all.sh --case platform/llvm-rvv
bash tests/run-all.sh --suite compiler
```

| Coverage | Checks |
|---|---|
| CPU and instruction cache | Guest boot, scalar/vector results, timing, fills, invalidation and precise failures |
| RVV and queues | Grouped/whole-register transfers, masks/tails, hazards, finite queues, fences and traps |
| SPM | Byte/range ordering, shared CPU/router bank permissions, bank ports and channel service |
| Analog accelerator | Register-only payloads, numerical outputs, program delay, overlapping computation and command backpressure |
| Mordred | 2×2 routing, payload integrity, local bank service, posted transfers, receive reservations and credit return |
| LLVM | Freshly compiled copy, addition, checksum and stencil kernels at two VLENs and two LSQ depths |
| Profiling | Default-off behavior, unchanged on/off timing and results, component traces and physical flit timing |
| Sculptor (optional compiler suite) | Compiler-generated single-tile linear/convolution programs, packing, tails and resident-weight reuse |

A hardware run builds test-only fixture components and compiles its guest inputs.
Results live under `tests/results/hardware/<run>/`; `results.json` lists every
suite and its log. Validators check numerical outputs and protocol/timing
invariants. A successful process exit alone is not the numerical validation.
The suite does not depend on or run performance studies. Add `--profile` to
collect optional component profiles; see [profiling controls](../docs/profiling.md).

The old `platform`, `memory`, `network` and `analog` test groups were retired with
the old simulator and are recoverable from [Git history](../docs/migration.md).
Other older runtime/validation research fixtures are outside the maintained
hardware suite. In particular, their legacy multi-tile runtime is not the new
NIU programming interface. Current guest-initiated networking remains follow-up work.
