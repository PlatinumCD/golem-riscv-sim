# Register-only instruction accounting

This test executes arithmetic only in registers inside a task-trace
start/finish interval:

- scalar: eight repeated `fadd.s` operations per iteration;
- RVV: one repeated `vfadd.vf` operation on eight `e32` lanes per iteration;
- iterations: `K = 10,000`.

There are no scratchpad accesses, memory accesses, DMA transfers, or NoC
operations in the measured interval. The runner requires
`scratchpad_service_cycles = 0` and records total elapsed cycles, guest CPU
cycles, measured-region counts, synchronization counts, and the non-guest
elapsed fraction in `results.csv`.

Only `measured_cycles` and `measured_instructions` use the task interval.
The scalar/vector totals and synchronization counters cover the whole run.
The CSV field `bridge_overhead_cycles` is calculated as total elapsed cycles
minus guest CPU cycles; it does not directly measure bridge service time.

Run it with:

```sh
./tests/platform/register-only-accounting/run-test.sh
```
