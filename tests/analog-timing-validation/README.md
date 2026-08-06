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
256-bit (eight-f32) half-duplex link at 1 GHz, and eight-cycle executes.

The analyzer takes command submission cycles from the QEMU/SST boundary and
feeds them into an independent Python reference scheduler. It then requires
exact agreement for every service phase timestamp, total active cycles, and
total link beats. It also checks that one device cycle is exactly one
nanosecond in SST. CPU work before or between submissions is therefore not
charged to analog device service.

Run:

```bash
./tests/analog-timing-validation/run-test.sh
```

Raw traces, logs, summaries, and the measured-versus-expected table are
written under `build/tests/analog-timing-validation/`.
