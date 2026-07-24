# Golem analog RISC-V translation

This directory contains the project-owned target code overlaid into the
pinned QEMU worktree.

- `trans_golem_analog.c.inc` translates the five `CUSTOM_0` instruction
  patterns and writes the helper status to `rd`.
- `golem_analog_helper.c` decodes operand meaning, snapshots or restores
  guest memory, and submits the command to `mittens-analog`.

The integration patch adds the instruction patterns to
`target/riscv/insn32.decode`, declares the TCG helper, and registers the
helper source. The complete encoding and retirement contract is recorded in
[`../../../docs/analog-isa.md`](../../../docs/analog-isa.md).
