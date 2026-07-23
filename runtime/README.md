# Golem bare-metal runtime

This directory builds the reusable, freestanding runtime library described in
[`../docs/platform-v0.md`](../docs/platform-v0.md). The target artifact is
`libgolem-runtime.a`, statically linked into each tile ELF alongside that
tile's generated registry, routes, tensor plan, and compiled MLIR task objects.

The library currently implements the agreed Platform v0 foundations:

- task and tensor ABI types;
- immutable binary-search task registries;
- the fixed 16-entry task-instance pool;
- the fixed FIFO ready queue;
- a transport-neutral interface that sends or receives one 32-bit word.

It deliberately does not provide an operating-system abstraction, heap
allocator, tensor partitioner, dynamic task mapper, or wider tensor transport.

Build and install the RISC-V archive with:

```bash
./build-scripts/build-runtime.sh
```

Run the host unit tests with:

```bash
./runtime/tests/run-test.sh
```

The installed layout is:

```text
install/runtime/include/golem/runtime/
install/runtime/lib/libgolem-runtime.a
```
