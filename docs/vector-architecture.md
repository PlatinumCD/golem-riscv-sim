# Vector architecture

The current CPU uses QEMU's RVV 1.0 execution with configurable VLEN. The model
supports VLEN 128, 256, 512 and 1024 bits. LMUL groups architectural vector
registers without multiplying the physical vector-to-array link width.

Eligible vector memory instructions, including whole-register and integer-LMUL
transfers, enter the finite LSQ as VLEN-sized beats. Masks, tails, memory ranges,
register dependencies and precise faults remain part of the execution contract;
unsupported asynchronous forms use the blocking path.

Read the [CPU contract](../src/components/riscv-qemu/README.md),
[LSQ contract](../src/components/riscv-qemu/load-store-queue.md), and
[register analog interface](../src/components/riscv-qemu/vector-analog.md).
Run `bash tests/run-all.sh --case platform/llvm-rvv` for freshly compiled C
kernels and `--case platform/vector-memory` for directed ISA checks.
