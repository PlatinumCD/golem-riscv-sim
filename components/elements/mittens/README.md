# Mittens

Mittens is the SST element for QEMU-backed, bare-metal RISC-V compute tiles.

The `mittens.tile` component owns the tile configuration and SST lifecycle and
exposes an optional `SST::Interfaces::SimpleNetwork` subcomponent slot. When
the slot is populated, the component forwards the SST `init`, `setup`, and
`finish` phases to the network interface.

With `launch_mode=managed`, the component launches one QEMU child process in
`setup()`, monitors it without blocking SST, and keeps the simulation alive
until QEMU exits. The default `launch_mode=disabled` is useful for element and
configuration tests. When `networkIF` is attached, the component creates a
per-tile shared-memory bridge and passes it to the custom QEMU `mittens-nic`
device. Bridge queues are polled at `process_poll_frequency`. Simulation-time
synchronization is not implemented; QEMU instruction progress remains
independent of SST time. See
[`../../../docs/timing-model.md`](../../../docs/timing-model.md) before using
simulation timestamps as performance measurements.

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
