# Hardware documentation

- [Current testing workflow](../testing.md)
- [Simulator source map](../../src/README.md)
- [Build and test tooling](../../tools/hardware/README.md)
- [Implementation invariants](refactor-invariants.md)
- [Task queue](task-queue.md)

## Preserved history

The files in `history/` document earlier source copies, refactors and acceptance
runs. Their historical commands and result paths describe those runs; they are
not the current build instructions.

- [Original source-refactor notes](history/source-refactor-notes.md)
- [Source-refactor acceptance](history/source-refactor-acceptance.md)
- [Cleanup audit](history/cleanup-audit.md)
- [Infrastructure acceptance](history/infrastructure-acceptance.md)
- [Simplification acceptance](history/simplification-acceptance.md)
- `history/source-path-map.json`: preserved mapping used only by the explicit
  historical copy-equivalence check.

The source-root cleanup relocated orchestration and documentation, not model
implementation. Component tests stay beside their code. Generated output paths
are unchanged; no historical data or reports were deleted.
