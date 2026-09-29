# Dependency integration patches

QEMU patches and overlays are owned by
[`components/riscv-qemu`](../components/riscv-qemu/build_qemu.py).
`llvm/` records the vector analog instruction definitions and compiler support
needed by current guests. `build-scripts/prepare-llvm.py` applies these to an
archive of the pinned commit without changing the developer checkout.
`torch-mlir/` contains its installed-MLIR integration patch. `sst-elements/`
contains the shared dependency's mesh VC fix; the current tile network is Mordred.
Mordred's unmodified vendored source and revision manifest live with its component.
