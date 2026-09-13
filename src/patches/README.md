# Dependency integration patches

These patches connect project-owned code to pinned upstream dependencies.
The implementation itself lives in [qemu/](../qemu/README.md),
[sst/](../sst/README.md), and [bridge/](../bridge/README.md).

| Directory | Applied by |
|---|---|
| [qemu/](qemu/) | [prepare-qemu.sh](../../build-scripts/prepare-qemu.sh) |
| [torch-mlir/](torch-mlir/) | [prepare-torch-mlir.sh](../../build-scripts/prepare-torch-mlir.sh) |

QEMU patches wire devices and instruction hooks into the machine, including
scratchpad boot, dynamic instruction fetches, and instruction-local RVV memory
transactions. Numeric prefixes are stable identifiers; gaps are not missing
dependencies. The preparation script defines the patch list and application order.

Keep changes against the revisions in
[versions.env](../config/build/versions.env). After editing, prepare and rebuild
through the repository scripts, then run the affected hardware tests.
