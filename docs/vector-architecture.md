# Vector architecture

QEMU executes RISC-V Vector Extension 1.0 instructions on each managed tile.
SST accounts for instruction issue and memory/device completion. RVV arithmetic
does not have a detailed pipeline timing model.

## Implemented configuration

| Property | Default tile / compiler target |
|---|---|
| ISA | RV64GCV with XGolemAnalog 1.0 |
| Compiler architecture | `rv64gcv_xgolemanalog` |
| ABI / byte order | `lp64d` / little-endian |
| Vector registers | 32 |
| VLEN / `vlenb` | 256 bits / 32 bytes |
| ELEN | 64 bits |
| CPU issue width | 1, 2, or 4; default 1 |
| Vector issue limit | One retired vector instruction per CPU cycle |

At LMUL=1, a register holds 32 eight-bit, 16 sixteen-bit, eight 32-bit,
or four 64-bit elements. Register grouping and fractional LMUL follow QEMU's
RVV semantics. SST does not derive arithmetic latency from VL, SEW, or LMUL.

The [QEMU launcher](../src/sst/execution/qemuProcess.cc) constructs:

```text
-cpu rv64,v=true,vext_spec=v1.0,vlen=256,elen=64
```

[Tile parameters](../src/sst/configuration/tileParameters.h) expose
`riscv_vector_enabled`, `riscv_vector_length_bits`, and
`riscv_vector_element_bits`. Alternate geometry needs a compatible compiler
target. [Startup](../src/platform/startup/crt0.S) enables `mstatus.FS` and
`mstatus.VS` before calling `tile_main`; generated code selects VL and VTYPE.

## Compiler and issue timing

LLVM's checked-in
[processor definition](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVProcessors.td)
gives `golem-analog` the V, Zvl256b, and XGolemAnalog features. The
[extension definition](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVFeatures.td)
declares XGolemAnalog 1.0. With the Golem compiler:

```sh
clang --target=riscv64-unknown-elf -mcpu=golem-analog -mabi=lp64d -O2 -c kernel.c
```

An additional `-march` is not needed to enable RVV. Automatic vectorization
depends on the loop and LLVM's profitability decisions. The processor uses
`NoSchedModel`; it has no Golem model for operation-specific vector resources.

The [CPU ledger](../src/sst/execution/cpuExecutionLedger.h) charges an issue
region as:

```text
cycles = max(ceil(total instructions / cpu_issue_width), vector instructions)
```

Occupancy carries across ordinary quantum ends; device boundaries close the
region. Vector configuration instructions count as vector instructions.
Arithmetic dependencies, operation latency, and vector-length-dependent
occupancy are not modeled.

## Memory, analog, and mesh

Vector loads and stores use guest memory. Enabled scratchpad accesses pass
through the bank/port timing model shared by CPU and DMA clients. Optional
StandardMem timing handles ordinary RAM accesses. Memory timing is implemented
separately from vector arithmetic timing; see the [timing model](timing-model.md).

Analog commands use general-purpose-register operands and memory buffers,
not an implicit vector-register connection. The
[analog device](../src/sst/analog/analogDevice.cc) shares one bidirectional
256-bit link across the tile's arrays: eight float32 words per beat.
This width does not establish RVV arithmetic latency or mesh bandwidth.

Mesh payload words and wormhole flits are 32 bits. Physical link width is
configurable in positive multiples of 32 bits, with separate clock, buffer,
and TX/RX lane settings. Software stores vector results before explicit
runtime/NIC transfers; registers are never transferred implicitly.

## Validation entry points

[RVV platform tests](../tests/platform/riscv-vector/run-test.sh) exercise
functional execution and compiler output.
[CPU timing tests](../tests/platform/cpu-timing/run-test.sh) exercise instruction
accounting and issue width. Neither establishes a detailed vector pipeline model.
