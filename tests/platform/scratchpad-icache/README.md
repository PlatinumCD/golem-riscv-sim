# Scratchpad-backed instruction-cache platform tests

Run from the repository root:

```sh
bash tools/hardware/env.sh bash tests/platform/scratchpad-icache/run-test.sh
```

The runner builds guest ELFs and executes them through SST with QEMU. It uses
one CPU tile, streaming memory, a 16 MiB global-RAM controller, no QEMU runtime
ready-set, and no local lookahead. Relevant mode configuration:

```text
scratchpad_boot=true
scratchpad_enabled=true
memory_backend=streaming
global_ram_bytes=16777216
instruction_cache_bytes=8192
instruction_cache_line_bytes=64
instruction_cache_ways=2
instruction_cache_hit_cycles=1
cpu_issue_width=1
qemu_ready_set_workers=1
qemu_runtime_ready_set=false
qemu_local_lookahead=false
```

Fixtures cover identical guest cycles and finish time at quanta 1/7/100;
two traversals of 20 KiB code exceeding the 8 KiB cache; a 32-bit instruction
straddling a 64-byte line; `fence.i` invalidation; executing modified code;
loading another kernel from shared RAM by DMA and executing it from SPM;
RVV load/add/store on separate SPM data; and a forbidden `0x80000000` load
whose handler requires `mcause=5` and `mtval=0x80000000`.

Every run requires successful guest exit, `SCRATCHPAD_ICACHE_PASS`, the
`SCRATCHPAD_BOOT` line, and the exact `INSTRUCTION_CACHE` counter line.
Guest binaries and measurements are saved under
`tests/results/scratchpad-icache/` (or the test runner's isolated results root).

The RVV fixture also checks the actual SPM beat trace: its aligned eight-word
load and store must each produce one 32-byte CPU transaction, with instruction
cache fills still present. It repeats at quanta 1, 7 and 100 and requires
identical instruction counts and finish time. Cross-instruction batching is
disabled, and its record limit is one, so vector width cannot depend on that
optimization. Program correctness alone does not pass this regression.
The faulting-vector fixture crosses the end of SPM after four valid elements;
it must reach its access-fault handler and retain identical accounting across
the same three instruction quanta.
