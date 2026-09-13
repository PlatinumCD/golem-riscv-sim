# Tile program support

These files are compiled into the RISC-V program. They issue device operations;
SST, not this code, determines their simulated timing.

| Directory | Contents |
|---|---|
| [startup/](startup/) | Entry code, parameterized SPM linker and simulation exit |
| [devices/](devices/) | UART, mesh NIC and shared-memory DMA helpers |
| [runtime/](runtime/) | Freestanding memory, math and runtime support |
| [deployment/](deployment/) | Adapters for generated deployment programs |

For an SPM-backed program, use [scratchpad.ld](startup/scratchpad.ld).
It links at `0x90000000`, fits code/data in 256 KiB SPM, and reserves a 16 KiB
stack. [crt0.S](startup/crt0.S) initializes the stack, clears BSS, enables
floating-point/vector state and calls `tile_main`.

The linker layout and simulated SPM capacity must agree. A program larger
than the 8 KiB instruction cache can run through refills; a program larger
than SPM cannot silently spill elsewhere. Override `SPM_BYTES`,
`SPM_CODE_OFFSET`, and `STACK_BYTES` with linker `--defsym` options before `-T`.
Fixed-address DMA fixtures reserve low SPM for data and place code above it.

Use [mesh-nic.h](devices/mesh-nic.h) for tile-to-tile communication and
[scratchpad-dma.h](devices/scratchpad-dma.h) for shared-memory transfers.
The complete address map and operation rules are in the
[programming interface](../../docs/platform.md).

Minimal correctness examples: [SPM/I-cache](../../tests/platform/scratchpad-icache/),
[data chunking](../../tests/memory/spm-chunking/), and
[mesh messages](../../tests/network/mesh-3x3/).
