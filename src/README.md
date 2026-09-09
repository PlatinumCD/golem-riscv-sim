# Simulator implementation

`src/` contains the active hardware model, guest platform support and integration
patches. Build orchestration, test runners and historical reports live outside
this directory.

| Directory | Responsibility |
|---|---|
| `bridge/` | Shared QEMU/SST protocol definitions |
| `config/` | Architecture presets and build configuration |
| `patches/` | Upstream QEMU and Torch-MLIR integration patches |
| `platform/` | Guest startup, device interfaces and runtime adapters |
| `qemu/` | Custom QEMU devices and instruction helpers |
| `sst/` | Tile, execution, memory, network, analog and profiling implementation |

Component correctness tests remain beside their owners, including `sst/tests/`.
Integration tests live in [`tests/`](../tests/) and architecture experiments in
[`studies/`](../studies/README.md).

## Build and test

```bash
JOBS=8 bash bootstrap.sh build-hardware
bash tests/run-all.sh --suite hardware
python3 -B tools/hardware/verify.py
```

These commands use `src/`, `build/src/` and `install/src/` by default. Source
organization does not change generated artifacts or architectural parameters.

- [Hardware tooling](../tools/hardware/README.md)
- [Testing guide](../docs/testing.md)
- [Hardware documentation and history](../docs/hardware/README.md)
