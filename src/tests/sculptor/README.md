# Sculptor compiler on one src tile

This test compiles tensor models through the real Sculptor pipeline and executes
the generated ELF through QEMU/SST. No handwritten MVM kernel or Python/C model
implementation is substituted for compiler output.

```sh
# Rebuild the compiler, preserving the checkout's local edits.
GOLEM_BUILD_SCOPE=shared GOLEM_SCULPTOR_SOURCE="$PWD/third_party/sculptor-mlir" \
  bash build-scripts/build-sculptor-mlir.sh
python3 -B src/tests/sculptor/run.py
```

The runner builds current SST components with `src/build.py`. To reuse a
fresh build, pass `--build-info /absolute/path/build.json`; source hashes must
match. QEMU defaults to `build/src/qemu/qemu-system-riscv64`. Build it with
`python3 -B src/components/riscv-qemu/build_qemu.py` when needed.

The four cases are:

| Case | Model | Array shape | Coverage |
|---|---|---|---|
| `linear_relu` | 2x13 input, 9x13 weights, ReLU | 17x19 | Weight/input/output transfer tails; two-sided padding; batched MVMs |
| `linear_sigmoid` | Same linear, sigmoid | 17x19 | Standard math lowering alongside new analog instructions |
| `linear_blocks` | Same linear, ReLU | 8x8 | Four packed arrays, cross-column reduction, row assembly |
| `conv_relu` | 1x1x5x5 input, two 3x3 kernels, ReLU | 8x8 | Sliding windows, two packed arrays, reuse across nine patches |

Every case initializes weights once and executes three different inputs, checking
all numerical outputs against an independent host reference. Defaults run VLEN
256 and 512 against the same ELF. Exact array traffic verifies that weights were
not reprogrammed between inputs; input payloads and guard bytes must remain
unchanged. The odd 17x19 hardware geometry exercises RVV chunk tails even after
compiler padding. Compiler regression tests separately verify nonzero memref
offsets, storage legality and emitted opcodes at O0 and O2.

```sh
python3 -B src/tests/sculptor/run.py --case linear_blocks --vlen 1024
python3 -B src/tests/sculptor/run.py --array-pipeline \
  --program-delay-scope initial_full_array
```

The optional array pipeline is tested for correctness with the generated
sequential schedule. This compiler change does not add overlap scheduling or
fuse analog vector results directly into the following activation.

The pipeline is canonicalize layers -> convert layers -> expand MVM -> tag and
construct RA -> cuts and execution groups -> initialize/distribute resources ->
operation/value ownership -> tile programs -> buffer and transfer plans ->
placement -> materialize deployment -> LLVM dialect -> LLVM IR -> RISC-V ELF.
Execution grouping permits the linear/convolution and its activation to occupy
one logical tile. Packed allocation binds up to four arrays there. The harness
rejects any plan requiring more than one tile.

`tools/compiler/sculptor_deployment/single_tile_runtime.cc` implements the fixed
input/output ABI using synchronous bounded local SPM copies. Inputs and outputs
are embedded in one writable payload image with compiler-defined offsets and
strides. There is no mesh, shared-RAM DMA, ACK or synthetic network on this path.
The computation remains entirely in the generated LLVM body. Code, constants,
payload, planned buffers and stack all fit within the same checked 2 MiB SPM.

Results contain source/expanded/program/resolved/LLVM MLIR, LLVM IR, the final
ELF and disassembly, exact build commands, deployment data, per-VLEN simulator
configuration and traces, validated outputs and counters. Total guest cycles
include startup, initialization and local input/output handling; they are not
isolated analog or digital-kernel performance measurements.
