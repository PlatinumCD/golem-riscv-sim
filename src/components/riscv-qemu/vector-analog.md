# Vector-register analog instructions

These local custom instructions connect RVV registers to the float32 analog
arrays. They are implemented by the project's LLVM assembler, the isolated
`src` QEMU build, and `tilecomponents.AnalogArrays`.

| Assembly | Effect | funct7 | Match |
|---|---|---:|---:|
| `mvm.vset vsrc, xarray, xoffset` | Program weights from vector registers | 6 | `0x0c00700b` |
| `mvm.vl vsrc, xarray, xoffset` | Load array inputs from vector registers | 7 | `0x0e00700b` |
| `mvm.vs vdst, xarray, xoffset` | Read array outputs into vector registers | 8 | `0x1000700b` |

All are 32-bit CUSTOM_0 instructions (`opcode=0x0b`, `funct3=7`, decoder mask
`0xfe00707f`). The vector register number occupies bits 11:7, `xarray` bits 19:15,
and `xoffset` bits 24:20. Unlike the old memory instructions, the first operand
names a vector register, not a scalar status register. `xarray` and `xoffset`
are RV64 scalar registers holding an array ID and an element offset; the full
64-bit values are checked without truncation. There is no immediate variant.

## Transfer contract

Use standard `vsetvli`/`vsetivli` to select SEW=32, a legal LMUL, and `vl`.
Each custom instruction transfers exactly `vl` float32 bit patterns, beginning
at vector element 0. LMUL selects the register group (including fractional
LMUL); integer groups must be aligned and fit within v0–v31. The instructions
are unmasked, require enabled RVV/VS and `vstart=0`, and preserve every
destination element beyond `vl`, including under tail-agnostic configuration.
They do not change `vl` or `vtype`.

Offsets are measured in elements. Weight offset `row * array_cols + column`
addresses the flat row-major matrix; contiguous chunks may cross rows. Input
and output offsets address their corresponding buffers. Require
`offset + vl <= capacity`, where capacity is `rows*cols`, `cols`, or `rows`.
Range checking avoids integer overflow. Zero-length transfers still validate
the array and range (allowing offset equal to capacity), preserve all state,
and consume no data-link or programming service.

Writes update only the specified elements. The array tracks initialized weight
and input elements, so a first MVM requires complete coverage of both buffers.
Coverage persists across later partial updates. In blocking mode, nonempty writes
invalidate the old computed output. In the default pipeline mode, input loads
preserve queued results and weight programming requires all results to be drained.
A nonempty `mvm.vs` waits for the oldest queued result to finish. No automatic
zero padding, implicit programming epoch, or final-chunk marker is used.

Use the existing `mvm xstatus, xarray, xarray` instruction to compute. In this
composition the default pipeline mode returns 0 after validated compute start,
or nonzero for an error detected before start. Explicit `array_pipeline_enabled=False`
makes it wait for completion. This existing instruction retains its original low-32-bit array ID
semantics. New vector instructions raise an illegal-instruction exception for
invalid RVV state, an invalid array/range, or an unavailable result/backend.
Failed transfers do not modify register or accelerator data.

## Timing and composition

```python
from configuration import connect_riscv_arrays

cpu, arrays, scratchpad = connect_riscv_arrays(
    sst, {"array_rows": 32, "array_cols": 32, "riscv_vector_length_bits": 256},
    elf="/absolute/path/to/guest.elf",
    memory_file="/absolute/path/to/run/scratchpad.bin",
)
```

With the default `analog_command_queue_depth=0`, each vector transfer blocks
until its command completes. The optional [analog command queue](analog-command-queue.md)
allows independent CPU instructions to continue after guaranteed admission. Register snapshots and
results travel through the local synchronization bridge v34 and SST command
events, with little-endian float32 payloads. They consume the existing shared
`array_link_width` bandwidth, duplex policy, finite in-flight buffer, and transit
latency. They issue no SPM requests themselves. The RVV instructions used to
load registers from SPM or write them back still consume normal CPU/SPM service.
With an enabled LSQ, legal analog instructions wait only for pending loads into
the vector registers they use. Unrelated loads and captured stores can complete
during the transfer. The [LSQ contract](load-store-queue.md#dependencies-and-precise-boundaries)
defines register grouping, preserved tails and precise trap handling.

The shared VLEN setting determines both register capacity and link bandwidth:
`array_link_width = riscv_vector_length_bits / 8`. VLEN 128/256/512/1024 gives
16/32/64/128 bytes per cycle. An uncontended link with enough in-flight buffering
can service one full LMUL=1 register each cycle. Integer LMUL groups contain
multiple registers and need proportionally more service cycles; LMUL does not
change the physical link width. Command transport and device latency are additional.

Each **nonempty `mvm.vset` chunk** pays `cost_per_array_program_cycles` once,
after its bytes cross the link. MVM uses `cost_per_mvm_cycles`. There is
no hidden matrix-level finalization cost. With the default analog command queue
disabled, vector analog transfers block individually while started MVM computations
are tracked independently. Arrays have no StandardMem slot or direct SPM path. The old
`mvm.set/l/s/mv` instructions trap immediately as illegal instructions; their
QEMU device and memory helpers are removed. All array data passes through
architectural vector registers. `connect()` creates independent test fixtures;
it provides no array-to-SPM connection. The old experimental overlap mode is
retired with the memory-based array interface.

## Array pipeline

The architecture parameter `array_pipeline_enabled=True` is the default.
Set it explicitly to `False` for blocking execution.
The composition sends the flag to both CPU and arrays. No new instruction
encoding or QEMU/LLVM build is required. This mode has an explicit asynchronous
`mvm` contract: status 0 means the array validated its initialized state, copied
the input into a protected snapshot, reserved a result slot, and **started**
computation. Errors detected before start still return nonzero status. There
are no deferred architectural errors after a successful start.

Each array has one compute engine and two result slots. The compute latency
remains `cost_per_mvm_cycles`; computation of two MVMs does not overlap.
Input preparation and output handling can run while that engine computes.
`mvm.vl` updates the input for the next job without changing the active snapshot
or buffered results. With `analog_command_queue_depth=0`, the CPU waits for
each analog register transfer; an enabled queue permits independent work while
protecting source and destination registers. Eligible RVV memory accesses can
use the LSQ independently. Shared link/bank
bandwidth limits continue to apply.

`mvm.vs` reads the oldest reserved result after it finishes. An enabled analog
queue may admit the instruction earlier when that result can be reserved safely;
its destination remains unavailable until the timed transfer completes.
Successfully reading every output element releases that result slot. Reads
may be split, reordered, or repeated; coverage counts unique elements. Once
the last unread element is read, subsequent stores select the next result.
A zero-length Store neither waits for nor consumes a result. Software must
finish draining the oldest result before submitting a third job, because a
full two-slot FIFO backpressures the next `mvm`. Partial result reads therefore
must eventually cover all rows. A nonempty `mvm.vset` is rejected while an
execution or buffered result remains; drain results before changing weights.

A typical schedule after programming weights is:

```text
load input 0; start MVM 0
for each next input n:
    load input n; start MVM n
    drain output n-1
drain final output
```

Memory/instruction fences, task markers, and guest exit wait for all started
computations. They do not consume buffered results. `mvm.vs` writes its vector
destination only after data is ready; input/output register hazards retain
normal instruction ordering. All traffic remains SPM ↔ CPU vector registers
↔ array. The [runtime benchmark](../../tests/single-tile-runtime/README.md)
selects this schedule with `--pipeline`; simply enabling the hardware flag
does not reorder an arbitrary guest loop.

## Compiler and tests

The patched LLVM tools accept assembly and C/LLVM inline assembly. No new C
builtins or LLVM intrinsics are introduced. Use:

```bash
install/llvm/bin/clang --target=riscv64-unknown-elf \
  -mcpu=golem-analog -fuse-ld=lld ...
python3 -B src/components/riscv-qemu/build_qemu.py
python3 -B src/tests/vector-analog/run.py
```

An explicit `-march` must include the custom extension or it can override the
CPU's feature selection. GNU assembler does not know these mnemonics. The
[test guest](../../tests/vector-analog/guest.c) provides inline-assembly chunk
loops and a trap handler. Each asm block sets the vector configuration, uses
the selected group, and declares all affected registers and memory effects.

Tests cover multiple arrays; VLEN 128/256/1024; m1/m2/m8 and fractional LMUL;
short chunks; the largest supported 1024-byte payload; partial reprogramming;
initialization coverage; zero-length transfers; invalid ranges, VS, SEW,
vstart, and group alignment; destination preservation; link/in-flight limits;
per-chunk programming latency; rejection of retired memory instructions before
accessing their address operands; and instruction-budget boundaries. Tests
inspect the actual SST graph and verify that slower SPM service leaves array
command latency unchanged. The host
independently verifies resulting SPM bytes. Reports are saved under
`tests/results/source-new-vector-analog/`.
