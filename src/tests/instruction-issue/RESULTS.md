# Instruction issue validation — 2026-10-02

The implementation supports in-order multiple issue with register readiness,
execution-unit capacity, result latency, initiation intervals and a bounded
instruction fetch buffer. Single issue remains the default. Enable dual issue
with `cpu_parameters={"issue_width": 2}`; the default fetch width follows it.
See the [timing contract](../../components/riscv-qemu/instruction-issue.md).

## Directed comparisons

Both columns use the new timing model and the same guest binary. Times below
are the span from first to last instruction admission, inclusive, in warmed
kernel regions. They exclude markers, setup and result stores.

| Kernel | Instructions | Single issue, cycles | Dual issue, cycles |
|---|---:|---:|---:|
| Independent integer adds | 128 | 128 | 64 |
| Dependent integer adds | 128 | 128 | 128 |
| Independent scalar/RVV adds | 128 | 128 | 64 |
| Dependent pointer-chase loads | 32 | 311 | 295 |
| Independent loads mixed with integer adds | 64 | 78 | 68 |
| Dependent multiplies | 32 | 94 | 94 |
| Dependent vector adds | 32 | 32 | 32 |

Restricting dual issue to one integer port or one fetched instruction restores
the independent-add span to 128 cycles. Increasing ALU latency to three cycles
makes the dependent-add span 382 cycles. Separately restricting the initiation
interval checks that result latency and port occupancy are distinct.

The pointer chase improves because the wider fetch buffer retains the next
instruction across a memory stall. Trace assertions establish that its next
load never issues before the previous read completes; some issue exactly at
completion. It does not overlap dependent reads.

Host synchronization budgets 1, 7 and 256 produce byte-identical instruction
issue, memory and instruction-cache traces for dual issue. All eleven cases
validate the same numerical results.

## Compiler-generated convolution + ReLU

Replayed the existing official first-convolution guest without recompiling it:
input `1×3×32×32`, 16 output channels, 1,024 patches and 16,384 output floats.
This uses the Large configuration: VLEN 512, eight 4-byte SPM banks, 2 MiB SPM,
scalar and vector LSQ depths 8, analog command depth 4, 100-cycle MVM latency
and zero programming delay. The deployment has four configured 512×512 arrays;
this kernel uses array 0 with a 16×27 active region. The existing 10×10 mesh
ran with 16 SST threads. Only CPU issue width and its following fetch width
changed between the two trials.

| Measurement | Single issue | Dual issue |
|---|---:|---:|
| Measured inference latency, cycles | 580,748 | 534,421 |
| CPU end cycle, including preceding work | 587,561 | 540,669 |
| Issued instructions | 225,478 | 225,478 |
| Cycles containing instruction issue | 225,478 | 188,336 |
| Peak instructions issued in one cycle | 1 | 2 |
| CPU memory requests | 111,914 | 111,914 |
| CPU data read bytes | 749,937 | 749,937 |
| CPU data written bytes | 498,708 | 498,708 |
| Instruction-cache misses | 163 | 163 |
| Instruction-cache fill bytes | 10,432 | 10,432 |

Dual issue reduces inference latency by **7.98%**, a **1.087×** speedup. Every
output is bit-identical to the archived official run, and the production
verifier passes for both trials. Memory dependencies and device service still
limit the application; doubling admission width does not halve its runtime.

This is a single/dual comparison within the new timing model. The older
archived model reported 579,608 inference cycles and did not enforce the new
arithmetic result timing, so it is a different baseline.

## Validation and artifacts

Passed: the native scheduling policy checks, 35 configuration tests, 11
instruction-issue cases, 10 instruction-cache cases, four scalar-LSQ cases,
three analog-register dependency cases, two vector-memory cases and the
dual-issue MVM → multi-hop transfer → MVM test. The two application trials
also pass. Component input/artifact hashes and QEMU input hashes match the
tested sources. QEMU and SST use bridge ABI 37 and must be rebuilt together.

Reproduce the maintained regression from the repository root:

```sh
bash tests/run-all.sh --suite hardware --case platform/instruction-issue
```

Raw artifacts are under `/tmp/golem-instruction-issue-20261002/`:

- `validated-focused/results.json`, `metadata.json` and per-case traces.
- `validated-cache`, `validated-scalar`, `validated-analog`, `validated-vector`
  and `validated-network` contain the supporting regression results.
- `application-comparison-validated.json` contains both application timings.
- `replay_application_validated.py` records the exact application replay.
- `validated-convolution-issue-1` and `validated-convolution-issue-2` contain
  the application configurations, build manifests, traces and verification.

The application guest comes from
`/tmp/sculptor-first-conv-relu-official-trace-20261002/compiled/official/resnet32_first_conv_relu`.
These temporary artifacts and the application input are local to this machine;
the maintained instruction-issue regression builds its own guest.

The execution-unit timings are configurable assumptions, not measurements of
a commercial CPU. RVV arithmetic latency is per instruction, with conservative
dependency sets for some vector forms. The model remains in order and has no
speculation, register renaming or separate retirement pipeline.

## Integration validation — 2026-10-03

All 24 maintained hardware suites pass with the current component build and
matching QEMU. Additional 4×4 mesh comparisons pass with 1, 8 and 16 SST threads,
including profiling at 16 threads. The LLVM analyzer distinguishes instruction
issue waits from memory waits. The analog instruction/trap regression verifies
that wider issue preserves instructions and traffic while reducing elapsed time;
changing the host synchronization budget still preserves simulated time exactly.

Evidence is in `/tmp/golem-sim-infrastructure-push-20261003/validation.json`.
