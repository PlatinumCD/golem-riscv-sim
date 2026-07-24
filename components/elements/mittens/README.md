# Mittens

Mittens is the SST element for QEMU-backed, bare-metal RISC-V compute tiles.

The `mittens.tile` component owns the tile configuration and SST lifecycle and
exposes an optional `SST::Interfaces::SimpleNetwork` subcomponent slot. When
the slot is populated, the component forwards the SST `init`, `setup`, and
`finish` phases to the network interface.

With `launch_mode=managed`, the component launches one QEMU child process in
`setup()`, keeps the simulation alive until QEMU exits, and controls that
child through the fd 41 synchronization bridge. SST grants precise icount
quanta and schedules the returned instruction count on `cpu_clock`; the
initial timing policy is one retired instruction per cycle. The default
`launch_mode=disabled` remains useful for element and configuration tests.
Normal guest completion is also an fd 41 event, so SST schedules and reaps
QEMU at the reported terminal instruction boundary instead of assigning time
to host-side process polling.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `cpu_clock` | SST clock for synchronized instruction cycles | `1GHz` |
| `sync_instruction_quantum` | Maximum instructions in one QEMU grant | `1000` |

When `networkIF` is attached, the component creates the fd 42 data bridge for
the custom QEMU `mittens-nic` device. A transmit publishes data on fd 42 and
yields through fd 41. See
[`../../../docs/timing-model.md`](../../../docs/timing-model.md) for modeled
timing and remaining CPU-model limits.

## Analog device

Setting `analog_array_count` to a nonzero value gives a tile one local
`AnalogDevice`. It supports both the native C++ float32 MVM backend and the
CrossSim backend. The managed QEMU child receives the analog bridge on file
descriptor 43 and submits compiler-emitted Golem instructions through it.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `analog_array_count` | Simulation-wide array count on every tile | `0` |
| `analog_array_rows` | Simulation-wide row count for every array | `100` |
| `analog_array_columns` | Simulation-wide column count for every array | `100` |
| `analog_backend` | Numerical backend: `native` or `crosssim` | `native` |
| `crosssim_config` | Optional CrossSim JSON parameter path or built-in configuration name | Empty/default CrossSim parameters |
| `analog_link_clock` | Common clock for every array's independent bidirectional 256-bit link | `1GHz` |
| `analog_compute_latency_cycles` | Compute latency in link-clock cycles | `100` |

The device implements `SetMatrix`, `LoadVector`, `Compute`, `StoreVector`, and
`MoveVector`. Every array owns an ordered four-command queue and an independent
bidirectional 256-bit link. Each active array advances up to eight float32
words in one direction per cycle, so aggregate bandwidth is
`analog_array_count * 256` bits per cycle when all arrays are active.
`StoreVector` consumes `ceil(analog_array_rows / 8)` link cycles before its
result is returned to QEMU.

`tests/analog_device_test.cpp` verifies the command semantics, MVM results,
array-to-array movement, error status, queue backpressure, bidirectional
transfer-cycle accounting, and concurrent progress on independent arrays.
`tests/analog_bridge_test.cpp` verifies the shared-memory ABI from an
independently mapped QEMU-side view.
`tests/sync_bridge_test.cpp` verifies fd 41 grant, analog-yield, resume,
quantum-end, and terminal-event state transitions from an independently
mapped QEMU-side view.
`tests/crosssim_backend_test.cpp` programs and computes through two independent
CrossSim backend instances and verifies that their local array state remains
separate.
`tests/analog_configuration.py` verifies that different tiles own the same
fixed array count and the same configured row and column counts. Backend
objects and array state remain independently owned by each tile. The proof
uses one SST global parameter set for the three geometry values and applies it
to every tile, then constructs the arrays with `analog_backend=crosssim`.

`tests/multi_tile_boot.py` creates two or more independently managed tiles in
one SST simulation. It defaults to four tiles and accepts the tile count from
`MITTENS_TEST_TILES`.

`tests/two_tile_bridge.py` connects two QEMU-backed tiles through
`merlin.linkcontrol` and a two-port Merlin router. Tile 0 sends a float32
payload to tile 1, tile 1 validates it and returns an acknowledgment, and both
guests exit only after the round trip succeeds.

The intended component boundary is:

```text
bare-metal ELF <-> QEMU <-> mittens.tile <-> merlin.linkcontrol <-> Merlin mesh
```

The ownership boundaries and complete packet path are documented in
[`../../../docs/architecture.md`](../../../docs/architecture.md).
