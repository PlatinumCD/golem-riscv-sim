# Golem analog RISC-V translation

This directory contains the project-owned target code overlaid into the
pinned QEMU worktree.

- `trans_golem_analog.c.inc` translates the five `CUSTOM_0` instruction
  patterns and writes the helper status to `rd`. Source operands go through
  QEMU's RISC-V register accessor so an encoded `x0`, including compiler-emitted
  constant array ID zero, becomes a valid TCG constant rather than an
  unallocated `cpu_gpr[0]`.
- `golem_analog_helper.c` decodes operand meaning, snapshots or restores
  guest memory, and submits the command to `mittens-analog`.

For `mvm.set`, `rs2[7:0]` is the array ID, `rs2[19:8]` is the valid
row count, and `rs2[31:20]` is the valid column count. A nonzero valid shape
sets `MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX` and transfers exactly
`rows * columns` row-major float32 words. The SST device reconstructs the
fixed physical array with zeros outside that valid rectangle. A zero row and
zero column retain the original full-physical-matrix wire form; only the
compact form is emitted by current compiler-generated programs.

During the one-time managed-QEMU memory-initialization interval, the helper
copies a compact matrix snapshot in one host operation and charges its exact
guest byte count through the normal aggregate memory-initialization timing
path. Outside initialization it retains the ordinary scalar guest loads, so
runtime-visible memory accesses are not bypassed.

The integration patch adds the instruction patterns to
`target/riscv/insn32.decode`, declares the TCG helper, and registers the
helper source. The complete encoding and retirement contract is recorded in
[`../../../../docs/analog-isa.md`](../../../../docs/analog-isa.md).
