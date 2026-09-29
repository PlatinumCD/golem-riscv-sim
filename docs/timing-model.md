# Timing model

QEMU supplies functional RISC-V execution; SST owns simulated time. Instruction
accounting, explicit memory responses, register dependencies, finite queues and
accelerator completion govern CPU progress. This is an in-order timing model,
not a detailed speculative or out-of-order processor.

SPM requests consume configured bank read/write ports and memory channels.
Range ordering prevents conflicting operations from becoming visible too early,
including QEMU's deferred functional commits. Instruction-cache refills use the
same bank service. Scalar operations block; eligible RVV transfers can overlap
through the LSQ while retaining register hazards and precise boundaries.

Array register transfers consume their own link bandwidth. Computation and
programming use configured delays; array payloads do not bypass CPU registers.
The compute pipeline defaults to enabled and requires an overlapping software
schedule to realize concurrency.

Mordred forwards finite-size packet pieces through credited input/output buffers
and a contended router switch. NIU local SPM requests use normal bank service.
Posted sends reserve destination storage before source acceptance; completion
is observed locally at the destination and storage credits return internally.

Detailed contracts and counters: [CPU](../src/components/riscv-qemu/README.md),
[LSQ](../src/components/riscv-qemu/load-store-queue.md),
[analog queue](../src/components/riscv-qemu/analog-command-queue.md),
[SPM](../src/components/scratchpad/README.md),
[NIU](../src/components/mordred/spm-interface.md).
