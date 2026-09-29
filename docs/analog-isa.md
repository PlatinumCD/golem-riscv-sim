# Analog instruction interface

The maintained instructions are `mvm.vset` (weights from vector registers),
`mvm.vl` (inputs from vector registers), `mvm` (compute), and `mvm.vs` (results to
vector registers). Array selectors, element offsets, counts, legality, errors,
register hazards and pipeline behavior are specified in the
[component instruction contract](../src/components/riscv-qemu/vector-analog.md).

Arrays have no direct scratchpad interface. Software surrounds analog register
transfers with ordinary RVV memory instructions as needed. The older memory-based
analog transfer instructions are unsupported and trap.

The LLVM instruction definitions are preserved in
[`src/patches/llvm`](../src/patches/llvm). The current QEMU implementation is
[`src/components/riscv-qemu/qemu`](../src/components/riscv-qemu/qemu).
