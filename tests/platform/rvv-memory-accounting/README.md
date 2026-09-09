# RVV memory-boundary accounting

Three exact 64-repetition assembly kernels test:

- `vle32.v` followed by a vector add.
- A fence followed by a vector add and `vse32.v`.
- Compressed `c.lw` / `c.sw` surrounding a vector add.

Each runs with instruction grants of 1, 2, 3, 7, 31, 127 and 1000. Every case
checks the functional result, exact total vector count (131, 131 or 67), equal
instructions/cycles for the one-issue core, and identical total/timed counts
across grant sizes. The three extra vectors are setup `vsetivli`, `vmv.v.i`
and post-timing consumption `vmv.x.s`. No compiler/model runtime is linked.

```bash
bash tests/platform/rvv-memory-accounting/run-test.sh
bash tests/run-all.sh --case platform/rvv-memory-accounting
```

An explicit `--qemu /path/to/pinned/qemu-system-riscv64` supports negative
reproduction with older hardware. Results and failed logs remain in isolated
run directories; timeout remains distinct from failed validation.
