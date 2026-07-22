# Mittens QEMU NIC

This directory contains the project-owned QEMU device used by the Mittens
SST element. The source-preparation script installs these files into the
pinned QEMU source tree before applying the integration patches.

Overlay destinations:

- `mittens_nic.c` -> `hw/misc/mittens_nic.c`
- `mittens_nic.h` -> `include/hw/misc/mittens_nic.h`

The shared QEMU/SST protocol is defined by `bridge/include/mittens/bridge.h`.
