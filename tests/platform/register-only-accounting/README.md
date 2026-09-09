# Register-only instruction accounting

This is Test 4. It executes arithmetic only in registers inside a task-trace
start/finish interval:

- scalar: eight repeated `fadd.s` operations per iteration;
- RVV: one repeated `vfadd.vf` operation on eight `e32` lanes per iteration;
- iterations: `K = 10,000`.

There are no scratchpad accesses, memory accesses, DMA transfers, or NoC
operations in the measured interval. The runner requires
`scratchpad_service_cycles = 0` and records total elapsed cycles, guest CPU
cycles, measured-region counts, synchronization counts, and the non-guest
elapsed fraction in `results.csv`.

Run it with:

```sh
./tests/platform/register-only-accounting/run-test.sh
```
