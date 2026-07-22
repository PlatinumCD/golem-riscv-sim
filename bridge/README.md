# Mittens host bridge ABI

`include/mittens/bridge.h` is the shared-memory contract between one QEMU
`mittens-nic` device and one SST `mittens.tile` component. It is intentionally
C-compatible so that the QEMU C device and the SST C++ element consume the
same declarations.

The QEMU process produces the transmit ring and consumes the receive ring.
The SST process consumes the transmit ring and produces the receive ring.
Both rings are bounded SPSC queues with monotonic 32-bit indices.
