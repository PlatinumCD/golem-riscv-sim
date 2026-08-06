# Shared L2 memory validation

This test uses two QEMU tiles. Each tile has one private L1 cache.
Both L1 caches connect to one logically shared L2 cache with two banks.

The test checks these properties:

- Each tile uses a separate timing-address namespace.
- The memory trace identifies the active task and execution.
- Both L2 banks exist in the SST statistics.
- A 300-word route completes through the receive-DMA path.
- The destination L1 invalidates the DMA destination lines before use.
