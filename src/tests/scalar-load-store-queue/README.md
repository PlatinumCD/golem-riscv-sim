# Scalar load/store queue regressions

Run `python3 -B src/tests/scalar-load-store-queue/run.py` after building matching
SST components and QEMU. The hardware suite also includes
`platform/scalar-load-store-queue`. `--case`, `--output`, `--build-info`,
`--qemu`, and `--compiler` select individual cases and build artifacts.

The same guest verifies integer signed/unsigned loads, store capture, compressed
loads/stores, FP32 NaN boxing, FP64 values, register hazards, partial memory
overlap, scalar/vector ordering, and scalar operands/results of RVV operations.
Misaligned/page-crossing accesses and a precise fault exercise the blocking
fallback. Older requests must retire before the first trap-handler fetch.

The suite tests the omitted default, disabled mode, depths 1/2/4/8/16/64,
instruction budget 1, issue width 4, and vector queue depth 1. It checks finite
capacity, response-before-retirement, in-order retirement, identical guest memory,
and identical event timing for default/explicit depth 8 and instruction budgets
1/256. Task snapshots report scalar queue stalls independently of vector stalls.

Two independent 8 KiB scalar transfer loops verify useful read/write overlap;
a serial pointer chain retains load-to-address dependencies. The standalone
linker uses a 2 MiB SPM with two 4-byte banks, VLEN 128, vector depth 8, and
analog depth 4. These are correctness tests with timing checks, not a study.

Existing vector queue, analog, network, and profiling suites run with the scalar
queue enabled by default and cover interaction with other memory clients,
analog computation, buffer ownership, and network backpressure.
