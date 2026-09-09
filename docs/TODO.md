# Documentation TODO

## Architecture documentation

- [x] Consolidate the platform description into `platform.md`.
- [ ] Update `timing-model.md` for multi-lane RX, streaming TX, bounded FIFOs,
  SPM arbitration, current defaults, and overlapping-counter semantics.
- [ ] Update `vector-architecture.md` to distinguish the fixed RVV contract
  from configurable simulator settings and document the implemented SPM path.
- [ ] Update `analog-isa.md` for compact `mvm.set` shapes and the actual
  operand interpretation.
- [ ] Replace the architecture SVGs and their README with diagrams of the
  current SPM, DMA, configurable TX/RX, and mesh paths.
- [ ] Recheck every link from `docs/README.md` after the rewrites.

## Documentation rules

- Architecture documents describe implemented interfaces and explicit limits.
- Study results, compiler plans, refactor notes, and historical task logs do
  not belong in `docs/`.
- Parameter defaults come from source; study-specific settings are labeled as
  study configurations.
- Resource service and stall counters are not summed as elapsed runtime.
