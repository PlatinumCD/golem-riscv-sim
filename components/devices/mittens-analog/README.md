# Mittens QEMU analog device

The `mittens-analog` QOM device is the QEMU side of a tile-local analog
accelerator. It has no guest MMIO window. The five Golem custom RISC-V
instructions call it through the helper in
[`../../qemu/golem-analog`](../../qemu/golem-analog).

An analog-enabled `mittens.tile` creates a geometry-sized shared `memfd`,
duplicates it to child descriptor 43, and launches QEMU with:

```text
-global mittens-analog.bridge-fd=43
```

During realization the device maps and validates the versioned
[`AnalogTileBridge`](../../../bridge/include/mittens/AnalogTileBridge.h)
header. Array count, rows, columns, queue capacity, payload size, dynamic
strides, and the 256-bit link width must all agree before execution begins.

`mvm.set` and `mvm.l` snapshot guest memory before publishing a command.
`mvm` and `mvm.mv` publish only array IDs. These operations wait for SST
acceptance and then retire. After publishing fd 43 data, every operation
yields through the separate fd 41 synchronization bridge. `mvm.s` remains
stopped on fd 41 until completion, copies the returned float32 words into
guest memory, and then retires. A reused full slot similarly yields on fd 41
until its prior completion, providing bounded per-array backpressure.

Every array has four slots and an independent write/accept sequence. Slot
state moves through `FREE`, `SUBMITTED`, `ACCEPTED`, and `COMPLETED` with
release/acquire ordering. Slot state is data-plane state only: SST resumes
QEMU through fd 41 after acceptance or completion. QEMU never interprets
numerical values; the native or CrossSim backend in the Mittens element owns
MVM semantics.
