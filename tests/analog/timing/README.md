# End-to-end analog timing validation

This gate runs real bare-metal RISC-V guests through the complete custom
analog path:

```text
Golem instruction
  -> QEMU decode/helper
  -> fd 43 command and payload
  -> fd 41 synchronization event
  -> SST AnalogDevice
  -> native numerical backend
  -> fd 43 completion
  -> QEMU guest result
```

The single-array case validates `set`, `load`, `execute`, and `store`. The
dual-array case additionally requires shared-link contention and overlapping
execution on independent array compute engines. Both use 9x9 arrays, a
256-bit (eight-f32) half-duplex link at 100 MHz, and 16-cycle executes.
These fixture timings leave enough time for the SPM-backed CPU to queue both
arrays while work is active; they are not the architecture defaults.

The analyzer takes command submission cycles from the QEMU/SST boundary and
feeds them into an independent Python reference scheduler. It then requires
exact agreement for every service phase timestamp, total active cycles, and
total link beats. It checks clock conversion at both interval endpoints:
one analog cycle is 10 ns, while CPU submissions may arrive between edges.
CPU work before or between submissions is not charged as analog service.

Run:

```bash
./tests/analog/timing/run-test.sh
```

Raw traces, logs, summaries, and the measured-versus-expected table are
written under `tests/results/analog-timing-validation/` by default.
