# Simulator implementation

`src/` contains the hardware model, guest platform support, and integration
patches. Build orchestration and test runners live outside this directory.

| Directory | Responsibility |
|---|---|
| `bridge/` | Shared QEMU/SST protocol definitions |
| `config/` | Architecture presets and build configuration |
| `patches/` | Upstream QEMU and Torch-MLIR integration patches |
| `platform/` | Guest startup, device interfaces and runtime adapters |
| `qemu/` | Custom QEMU devices and instruction helpers |
| `sst/` | Tile, execution, memory, network, analog and profiling implementation |

Component correctness tests remain beside their owners, including `sst/tests/`.
Integration tests live in [`tests/`](../tests/). Architecture studies are kept
locally under `studies/` and are not included in Git.

## Build and test

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
python3 -B tools/hardware/verify.py
```

Run these commands from the repository root, with shared dependencies installed.
Hardware outputs default to `build/src/` and `install/src/`. The complete suite
currently requires a local study dependency; see the
[communication-envelope wrapper](../tests/network/communication-envelope/README.md).

- [Hardware tooling](../tools/hardware/README.md)
- [Architecture documentation](../docs/README.md)
