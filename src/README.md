# Simulator implementation

`src/` contains the hardware model, guest platform support, and integration
patches. Build orchestration and test runners live outside this directory.

| Directory | Responsibility |
|---|---|
| [sst/](sst/README.md) | Simulated CPU, memory, network and analog timing |
| [qemu/](qemu/README.md) | Functional instruction execution hooks and devices |
| [platform/](platform/README.md) | Tile program startup, linker layouts and device helpers |
| [bridge/](bridge/README.md) | Shared QEMU/SST message formats |
| [patches/](patches/README.md) | Connect project-owned code to upstream dependencies |
| [config/build/](config/build/) | Toolchain settings and pinned dependency revisions |

Component correctness tests remain beside their owners, including `sst/tests/`.
Integration tests live in [`tests/`](../tests/README.md).

## Where to start in the code

| If you are changing… | Start here |
|---|---|
| The machine's defaults or valid configurations | [tileParameters.h](sst/configuration/tileParameters.h), [tileConfiguration.cc](sst/configuration/tileConfiguration.cc) |
| Program loading or instruction fetch | [scratchpadBootImage.cc](sst/memory/scratchpadBootImage.cc), [instructionCache.cc](sst/memory/instructionCache.cc) |
| Memory bandwidth or bank conflicts | [scratchpadTimingModel.cc](sst/memory/scratchpad/scratchpadTimingModel.cc) |
| CPU cycle accounting | [cpuExecutionController.cc](sst/execution/cpuExecutionController.cc), [cpuExecutionLedger.h](sst/execution/cpuExecutionLedger.h) |
| Send/receive concurrency or routing | [network/tx/](sst/network/tx/), [network/rx/](sst/network/rx/), [wormholeRouter.cc](sst/network/wormholeRouter.cc) |
| Tile wiring and lifecycle | [tile.cc](sst/tile/tile.cc) |

Edit these source files, not generated copies under `build/` or `install/`.
Resource controllers own scheduling; `tile.cc` connects them. Build scripts
and test runners should not duplicate the hardware model.

## Build and test

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
python3 -B tools/hardware/verify.py
```

Run these commands from the repository root, with shared dependencies installed.
Hardware outputs default to `build/src/` and `install/src/`.
The [test guide](../tests/README.md) lists hardware checks and their entry points.
Each run records execution and measurement validation separately.

- [Hardware tooling](../tools/hardware/README.md)
- [Architecture documentation](../docs/README.md)
