# Golem vector architecture

This document is the vector-processor contract for a conforming Golem tile. It
fixes the architectural vector geometry and the
boundary between the scalar CPU, standard RISC-V Vector Extension, custom
Golem analog accelerator, QEMU functional execution, and SST timing model.

The words **must**, **must not**, **should**, and **may** describe required,
forbidden, recommended, and optional behavior respectively.

## Fixed architectural contract

| Property | Current Golem value |
| --- | --- |
| Base ISA | RV64 |
| Standard ISA | `RV64GCV` |
| Custom ISA | `XGolemAnalog` 1.0 |
| Compiler architecture spelling | `rv64gcv_xgolemanalog` |
| Vector specification | RISC-V Vector Extension 1.0 |
| `VLEN` | 256 bits |
| `vlenb` CSR | 32 bytes |
| `ELEN` | 64 bits |
| Vector registers | 32 registers, 256 bits each |
| Vector datapath | 256 bits |
| Vector issue width | At most one vector instruction per CPU cycle |
| Byte order | Little-endian |
| C ABI | `lp64d` |

A conforming tile must implement standard RVV 1.0 behavior. The custom analog
instructions remain a separate extension and do not redefine RVV instruction
encodings, vector CSRs, masking, tail policy, or register grouping.

## Vector geometry

One `LMUL=1` vector register contains:

```text
SEW=8:   32 elements
SEW=16:  16 elements
SEW=32:   8 elements
SEW=64:   4 elements
```

The implementation supports the standard RVV register-group rules.
Non-fractional groups contain:

```text
LMUL=1:   256 architectural bits
LMUL=2:   512 architectural bits
LMUL=4:  1024 architectural bits
LMUL=8:  2048 architectural bits
```

`LMUL` changes the architectural register-group size; it does not widen the
physical 256-bit datapath. The eventual timing model must therefore account
for the number of physical datapath beats required by the active `VL`, `SEW`,
and `LMUL`.

## Scalar and vector issue

The scalar front end may be configured as single-, dual-, or quad-issue. The
vector unit has one vector issue slot:

```text
                         shared instruction front end
                    issue width = 1, 2, or 4 instructions
                                      |
                 +--------------------+--------------------+
                 |                                         |
          scalar resources                         one vector issue slot
       integer / FP / load-store                    per CPU cycle maximum
```

A vector instruction consumes one front-end issue slot and the vector issue
slot. Increasing scalar issue width must not imply that two or four vector
instructions can issue in one cycle. Independent vector operations may be
pipelined, but operation-specific latency and resource occupancy belong to
the Golem scheduling and SST timing models.

The simulator exposes the scalar front-end width as `cpu_issue_width`, with
supported values 1, 2, and 4. QEMU reports both total retired instructions and
retired RVV instructions at every fd 41 synchronization boundary. SST charges:

```text
cpu cycles =
    max(ceil(total retired instructions / cpu_issue_width),
        retired vector instructions)
```

The second term preserves the one-vector-issue-per-cycle limit when the scalar
front end is widened. This is an issue-throughput model, not yet a detailed
pipeline model: dependencies, operation-specific latency, vector-length
occupancy, and memory-system timing remain future work.

## Vector execution resources

The processor model must distinguish at least:

- vector configuration operations such as `vsetvli`;
- vector integer and mask arithmetic;
- vector floating-point arithmetic;
- vector multiply and fused multiply-add;
- vector reductions and permutations; and
- vector loads and stores.

Exact latency, initiation interval, and load/store bandwidth are not fixed in
this version of the contract. They must be explicit timing-model parameters
until backed by a hardware implementation. A conforming performance result
must not treat every RVV operation as a permanently fixed one-cycle operation.

The 256-bit datapath establishes the amount of vector data processed by one
full-width physical beat:

```text
32 x i8     16 x i16      8 x i32/f32      4 x i64/f64
```

It does not establish cache, scratchpad, or external-memory bandwidth. Those
remain separate memory-system properties.

## Analog accelerator relationship

All analog arrays on one tile share one bidirectional 256-bit link. The equal
RVV and analog-link widths are intentional:

```text
RVV datapath:            256 bits = 8 float32 values
Shared tile analog link: 256 bits = 8 float32 words
```

This equality does not create a direct architectural connection between a
vector register and an analog array, and it does not multiply analog
bandwidth by the number of arrays. The platform continues to use the
memory-based Golem analog commands:

```text
RVV load/store path:
    private RAM <-> vector registers

Analog command path:
    private RAM -> mvm.l -> analog array
    private RAM <- mvm.s <- analog array
```

`mvm.set`, `mvm.l`, `mvm`, `mvm.s`, and `mvm.mv` remain asynchronous
array-command operations with general-purpose-register operands. Direct
vector-register-to-array instructions would be a future ISA revision and
must not be assumed by software or compiler lowering.

## Mesh and task-runtime invariants

RVV is tile-local and does not change the network contract:

- one mesh channel transfer remains exactly 32 bits;
- tensor payloads remain ordered sequences of independent 32-bit words;
- route headers, task IDs, execution IDs, and runtime slots are unchanged;
- RVV registers are not transferred implicitly between tiles; and
- vectorized local computation must explicitly store data before the existing
  runtime or NIC can route it.

The tile task and tensor ABI remains memory-based. The platform introduces no
platform-specific vector calling convention and relies on LLVM's RISC-V ABI
behavior for compiler-generated code.

## Bare-metal execution contract

QEMU is the functional RVV executor. A conforming managed tile must launch
QEMU with:

```text
-cpu rv64,v=true,vext_spec=v1.0,vlen=256,elen=64
```

Before entering `tile_main`, `src/platform/crt0.S` must:

1. enable floating-point state through `mstatus.FS`;
2. enable vector state through `mstatus.VS`; and
3. leave `VL` and `VTYPE` selection to generated or explicit RVV code.

There is no operating system and no vector context switching. Each tile owns
one hart and one bare-metal program for the lifetime of the simulation.

Mittens exposes `riscv_vector_enabled`, `riscv_vector_length_bits`, and
`riscv_vector_element_bits` for validation and architectural experiments.
A conforming current run must use `true`, `256`, and `64`. Other accepted
values describe an experimental processor configuration and require a matching
compiler target.

## Compiler contract

The LLVM `golem-analog` processor definition must include:

```text
FeatureStdExtV
FeatureStdExtZvl256b
FeatureVendorXGolemAnalog
```

`FeatureStdExtV` provides the full standard vector extension and its required
dependencies. `FeatureStdExtZvl256b` records the minimum architectural vector
length used by the compiler. Once the compiler stage is complete, this must
be sufficient to select the full tile ISA:

```bash
clang \
  --target=riscv64-unknown-elf \
  -mcpu=golem-analog \
  -mabi=lp64d
```

An explicit `-march=rv64gcv_xgolemanalog` is permitted but must not be
required. Enabling RVV makes vector instructions legal; `-O2` or `-O3` and
LLVM's profitability analysis still determine whether an ordinary loop is
auto-vectorized.

The current `NoSchedModel` is temporary. A later compiler stage must introduce
a Golem scheduling model that represents the scalar issue width, single
vector issue slot, vector execution resources, and custom analog-command
resource.

## Timing ownership

QEMU owns instruction semantics and architectural state. SST owns simulated
time. Functional RVV support alone does not establish vector performance.

The completed functional path is:

```text
RVV ELF -> QEMU RVV 1.0 decode and execution -> retired instruction count
                                                |
                                                v
                           current SST scalar/vector issue-throughput model
```

The target timing path is:

```text
QEMU instruction accounting
  -> scalar or vector operation class
  -> VL / SEW / LMUL and memory-transfer metadata where required
  -> SST issue, latency, occupancy, and bandwidth model
  -> simulated completion cycle
```

Changing host QEMU execution speed must not change simulated vector time.

## Required acceptance proofs

The vector architecture is considered implemented only when all of the
following pass:

1. `vlenb` reads exactly 32 on a conforming tile.
2. Explicit RVV integer and floating-point operations produce correct output.
3. `-mcpu=golem-analog` emits RVV without an additional `-march`.
4. At least one ordinary C++ loop is auto-vectorized and verified in QEMU.
5. Existing scalar, custom analog, mesh, and deployment tests remain valid.
6. Scalar issue-width changes do not silently multiply vector issue width.
7. SST reports vector timing according to documented operation parameters
   rather than treating vector work as unclassified scalar work.

Items 1 through 6 are covered by the RISC-V vector microtest, compiler
code-generation checks, vectorized ResNet-18 deployment, and scalar/vector
issue-width comparison. Item 7 remains future work: the current model
preserves the vector issue limit but does not yet assign distinct latency and
occupancy to individual vector operations.

## Explicit non-goals

- New custom vector opcodes.
- Direct RVV-register/analog-array transfers.
- Wider mesh words or vector-sized network packets.
- Linux vector context management.
- A cache or scratchpad timing hierarchy.
- Uncalibrated claims that RVV operations complete in one cycle.
