# Golem analog RISC-V instruction contract

This document describes the implemented XGolemAnalog 1.0 instructions.
LLVM defines the encodings; the
[QEMU helper](../src/qemu/instructions/golem-analog/golem_analog_helper.c)
and [analog device](../src/sst/analog/analogDevice.cc) define operand
interpretation and execution. The
[analog bridge](../src/bridge/include/mittens/AnalogTileBridge.h) remains
protocol version 1.

The definitions inspected for this contract are:

- [`RISCVInstrInfoXGolem.td`](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVInstrInfoXGolem.td),
  which defines the instructions and selection patterns;
- [`IntrinsicsRISCVXGolem.td`](../third_party/llvm-project/llvm/include/llvm/IR/IntrinsicsRISCVXGolem.td),
  which defines the LLVM IR interface;
- [`RISCVISelLowering.cpp`](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVISelLowering.cpp),
  which lowers intrinsic operands to RISC-V registers;
- [`RISCVInstrFormats.td`](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVInstrFormats.td),
  which assigns `CUSTOM_0`; and
- [`RISCVFeatures.td`](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVFeatures.td)
  and [`RISCVProcessors.td`](../third_party/llvm-project/llvm/lib/Target/RISCV/RISCVProcessors.td),
  which define the extension and the `golem-analog` CPU.

LLVM identifies the vendor extension as `xgolemanalog` version 1.0.
`-mcpu=golem-analog` selects the RV64 processor definition and enables the
extension; `-mattr=+xgolemanalog` can enable it directly in LLVM tools.

## Common encoding

All five operations are 32-bit R-type instructions in the RISC-V
`CUSTOM_0` opcode space:

```text
 31          25 24       20 19       15 14    12 11        7 6          0
+--------------+-----------+-----------+--------+------------+------------+
|    funct7    |    rs2    |    rs1    |  111   |     rd     |  0001011   |
+--------------+-----------+-----------+--------+------------+------------+
```

The fixed-field decoder mask is `0xfe00707f`. For a 32-bit instruction word
`insn`:

```text
rd   = (insn >> 7)  & 0x1f
rs1  = (insn >> 15) & 0x1f
rs2  = (insn >> 20) & 0x1f
op   = (insn >> 25) & 0x7f
```

The instruction matches one of the rows below when
`(insn & 0xfe00707f) == match`.

## Instruction table

An address is an RV64 guest address in the `rs1` GPR. An array ID is a
tile-local, zero-based 32-bit identifier; it is not a mesh tile ID. Matrix and
physical extents come from `analog_array_rows` and `analog_array_columns`.
On the default architecture, address operands refer to SPM. Input data held
in shared main memory must first be transferred into a valid SPM range.
`mvm.set` additionally accepts a compact valid rectangle packed into the
value of `rs2`; this does not change the instruction encoding.

| Assembly | `funct7` | Match | `rs1` | `rs2` | Mittens operation and memory effect |
| --- | ---: | ---: | --- | --- | --- |
| `mvm.set rd, rs1, rs2` | `0x01` | `0x0200700b` | Matrix source address | Packed array ID / valid shape | Program a row-major float32 matrix; reads physical or compact rectangle bytes as specified below |
| `mvm.l rd, rs1, rs2` | `0x02` | `0x0400700b` | Input-vector source address | Destination array ID | Load a float32 input vector; reads `columns * 4` bytes from tile memory |
| `mvm rd, rs1, rs2` | `0x03` | `0x0600700b` | Array ID | LLVM duplicates `rs1` here | Compute that array's matrix-vector product; no guest-memory access |
| `mvm.s rd, rs1, rs2` | `0x04` | `0x0800700b` | Output-vector destination address | Source array ID | Store the float32 output vector; writes `rows * 4` bytes to tile memory |
| `mvm.mv rd, rs1, rs2` | `0x05` | `0x0a00700b` | Source array ID | Destination array ID | Move the source array's output into the destination array's input; no guest-memory access |

The byte counts and float32 interpretation in the last column are the
Mittens platform contract layered on the instruction encodings. LLVM itself
only identifies which instructions may read or write memory; it does not
encode an element type or transfer length.

### Compact matrix programming

QEMU consumes the low 32 bits of `mvm.set`'s `rs2` value:

| Bits | Meaning |
|---|---|
| 7:0 | Destination array ID |
| 19:8 | Valid rows (12 bits) |
| 31:20 | Valid columns (12 bits) |

Both shape fields zero select a full physical matrix. Both nonzero select
a tightly packed row-major rectangle of `valid_rows * valid_columns`
float32 words at `rs1`. Exactly one zero field, or a valid extent larger
than the physical array, returns `INVALID_PAYLOAD`.

The SST device expands compact data into the top-left rectangle of a
zero-filled physical matrix and remembers the valid extents. Matrix link
traffic includes only compact words. Subsequent `mvm.l` still snapshots
`physical_columns` guest words, but transfers only `valid_columns` over
the analog link; every omitted input word must be bitwise positive zero.
`mvm.s` transfers `valid_rows` over the link while still writing
`physical_rows` words to guest memory. Compact shape changes link work,
not the configured compute latency or the guest vector-buffer extents.

`mvm.mv` requires the source output extent to equal the destination input
extent. Because configured arrays on a tile share the same geometry, this currently
requires `analog_array_rows == analog_array_columns`.

## LLVM intrinsic interface

| LLVM IR intrinsic | Signature | LLVM memory properties | Register lowering |
| --- | --- | --- | --- |
| `llvm.riscv.golem.analog.mvm.set` | `void (ptr, i32 packed_operand)` | Has side effects, reads memory | `rs1 = ptr`, `rs2 = packed_operand`, `rd = x0` |
| `llvm.riscv.golem.analog.mvm.load` | `void (ptr, i32 array_id)` | Has side effects, reads memory | `rs1 = ptr`, `rs2 = array_id`, `rd = x0` |
| `llvm.riscv.golem.analog.mvm` | `void (i32 array_id)` | Has side effects | `rs1 = array_id`, `rs2 = array_id`, `rd = x0` |
| `llvm.riscv.golem.analog.mvm.store` | `void (ptr, i32 array_id)` | Has side effects, writes memory | `rs1 = ptr`, `rs2 = array_id`, `rd = x0` |
| `llvm.riscv.golem.analog.mvm.move` | `void (i32 source_id, i32 destination_id)` | Has side effects | `rs1 = source_id`, `rs2 = destination_id`, `rd = x0` |

LLVM names the ID variables `Tile` in its lowering code. Existing Golem
behavior interprets these values as local analog array IDs, except for the
packed `mvm.set` operand described above.

On RV64, LLVM promotes each intrinsic's `i32` array ID to XLEN using
`ANY_EXTEND`. QEMU must therefore consume the low 32 bits of an ID register;
the upper 32 bits are not defined by this intrinsic lowering.

Every intrinsic returns `void`. Although the assembly form contains an `rd`
field, compiler-generated instructions set `rd` to `x0`, so LLVM-generated
code cannot observe a completion status. Guest inline assembly can use a
nonzero `rd` and expects zero for success and nonzero for failure, but LLVM
does not define the exact status ABI. Mittens writes a documented
`MittensAnalogStatus` value when `rd` is nonzero. For asynchronous operations,
zero means the command was accepted; a backend error that occurs after
retirement remains in the completed bridge slot and is logged when QEMU
reclaims that slot rather than retroactively changing `rd`. A blocking
`mvm.s` reports its final completion status.
Compiler-generated code sets `rd = x0` and discards every status.

## Ordering and completion

The LLVM nodes are chain-carrying operations with side effects. `mvm.set` and
`mvm.l` are marked as loads, and `mvm.s` is marked as a store. This prevents
the compiler from treating the operations as removable pure computation and
expresses their guest-memory ordering requirements.

LLVM does not define accelerator latency, asynchronous queues, or instruction
retirement behavior. The platform supplies the asynchronous execution
contract:

- every analog array has an independent ordered command stream;
- all array transfers share one tile-wide bidirectional 256-bit link;
- commands for different array IDs may compute concurrently, while their
  transfers are arbitrated one 256-bit beat per tile per cycle;
- `mvm.set` and `mvm.l` retire after QEMU snapshots their guest-memory source
  and the selected array queue accepts the command;
- `mvm` retires after queue acceptance;
- `mvm.s` waits only for the selected array and retires after writing its
  output to guest memory;
- `mvm.mv` installs a dependency between its source and destination array
  streams; and
- a full queue applies backpressure only to the targeted array, or to either
  source/destination stream for `mvm.mv`.

Commands within one array stream remain in issue order, so the array ID is
sufficient for per-array dependency tracking and no command ID is encoded.
SST remains responsible for each array's modeled transfer and compute time.
With `N` active arrays, the tile still advances at most one 256-bit beat in
one direction during one cycle. Maximum aggregate tile-to-array bandwidth is
therefore always 256 bits per cycle, independent of `N`. Round-robin
beat arbitration prevents one active array from starving the others. In
particular, an uncontended `mvm.s` uses `ceil(valid_rows / 8)`
array-to-tile link cycles (physical rows after full-matrix programming);
contention adds waiting cycles.
Software must issue independent array work before a blocking `mvm.s`; the
single guest hart cannot issue later instructions while that join is waiting,
although every previously submitted array command continues in SST.

## Encoding examples

These words follow directly from the fixed fields and register indices:

| Assembly input | Little-endian bytes | 32-bit word |
| --- | --- | ---: |
| `mvm.set x5, x10, x11` | `8b 72 b5 02` | `0x02b5728b` |
| `mvm.l x6, x12, x13` | `0b 73 d6 04` | `0x04d6730b` |
| `mvm x7, x14, x15` | `8b 73 f7 06` | `0x06f7738b` |
| `mvm.s x8, x16, x17` | `0b 74 18 09` | `0x0918740b` |
| `mvm.mv x9, x18, x19` | `8b 74 39 0b` | `0x0b39748b` |

## QEMU decoder

The QEMU decodetree patterns implement the equivalent of one mask and five
matches:

```c
#define MASK_XGOLEM_ANALOG      UINT32_C(0xfe00707f)
#define MATCH_MVM_SET           UINT32_C(0x0200700b)
#define MATCH_MVM_LOAD          UINT32_C(0x0400700b)
#define MATCH_MVM_COMPUTE       UINT32_C(0x0600700b)
#define MATCH_MVM_STORE         UINT32_C(0x0800700b)
#define MATCH_MVM_MOVE          UINT32_C(0x0a00700b)
```

The instruction word encodes register indices, not immediate shapes or
transfer lengths. The `mvm.set` register value carries the valid shape;
physical geometry and backend configuration remain platform settings.
