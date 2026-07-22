# Testing

Each complete system scenario owns its sources, SST configuration when
needed, and a foreground `run-test.sh`.

## Single tile

```bash
./tests/hello/run-test.sh
```

This builds one bare-metal ELF, boots it directly with the repository QEMU,
prints `Golem Platform v0: single tile booted`, and exits successfully.

## Two-tile mesh

```bash
./tests/mesh-pair/run-test.sh
```

This builds distinct tile images, launches two QEMU processes through
`mittens.tile`, sends the float32 bit pattern for `3.25` through Merlin, and
returns an acknowledgment to tile 0.

## Mittens element tests

```bash
./components/elements/mittens/tests/run-test.sh
```

The element-local runner exercises component registration, managed single-tile
boot, four independently managed tiles, and the two-tile bridge.

All runners remain in the foreground and return a nonzero status on failure.
