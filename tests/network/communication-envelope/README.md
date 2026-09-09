# Communication envelope regression

Nine fixed hardware cases from the organized
[communication envelope study](../../../studies/compute-communication/communication-envelope/README.md):
delayed RX, T2 fan-out, same-link T4, full duplex, integrated RVV+TX+RX,
both repeated bank-phase controls, 16 KiB-plus-tail descriptors, and bounded-queue backpressure. This entry point
does not run the complete study or any compiler/model workload.

```bash
bash tests/network/communication-envelope/run-test.sh
bash tests/run-all.sh --case network/communication-envelope
```

The RVV and bank-phase cases retain coverage for the repaired QEMU accounting
and injection-lane ordering failures. The runner retains failed logs, continues
independent cases, and returns nonzero on any required failure. Passing transfer
and accounting checks must not be confused with demonstrating peak client rates.
