# Single-tile runtime comparison

Run from the repository root:

```bash
python3 -B src/tests/single-tile-runtime/run.py
```

This builds the SST component and two LLVM bare-metal guests, then runs all
four combinations of array dimension (32 or 64) and VLEN (256 or 512).
The isolated QEMU must already exist; build it using
`python3 -B src/components/riscv-qemu/build_qemu.py` if needed.
Results go to `tests/results/source-new-single-tile-runtime/<timestamp>/`.
`--output PATH` selects a directory and `--repeats N` changes the default 1,000
resident-weight MVMs (1–1,000 supported). All 1,001 outputs, including the first
MVM, occupy 128,128 bytes for the 32×32 case or 256,256 bytes for the 64×64 case
in SPM, below the guest stack.
`--build-info PATH/build.json` reuses a compiled component only when its complete
component source/header set and hashes match, including instruction-cache
sources and build/generator inputs. `--uncached` disables the instruction cache
for a controlled comparison; all other benchmark settings remain the same.
Array pipelining is enabled by default, with a matching two-job guest schedule.
`--pipeline` explicitly selects it; `--no-pipeline` measures the blocking control.
`--spm-banks N` changes the bank count for all four cases while retaining
four-byte banks, the SPM capacity and the channel settings. For example,
`--pipeline --spm-banks 4` compares four-bank throughput against the default
eight-bank configuration using the same guest programs.

Each tile has one physical array, and the float32 matrix matches its size:
32×32 or 64×64. All weights fit in array 0 and remain resident after programming.
These are different workloads: a 64×64 MVM performs four times the
multiply-accumulates of a 32×32 MVM. The larger array also has four times the
weight capacity. By default input preparation and output handling can overlap
array computation through the pipelined CPU interface.

Weights are `W[r,c] = ((3*r + 5*c) % 11) - 5`; inputs are `x[c] = (c % 5) - 2`.
These integer-valued floats give exact results in the array and scalar oracle.
All repeated MVMs use this same input. Each result is written to its own
32- or 64-float SPM output buffer and checked independently by the guest and host.

Phases use existing task markers, timestamped by SST:

| Task ID | Region |
|---|---|
| 0 | Empty pair, to quantify marker overhead |
| 1 | Program all 1,024 or 4,096 weights through ordinary RVV loads and `mvm.vset` |
| 2 | First full MVM, including inputs, one array execution, and outputs |
| 3 | Batch of repeated MVMs using resident weights; divide elapsed cycles by repeat count |

Cold timing spans task 1 start through task 2 finish, including the markers
between them. Initialization and correctness checks are outside
the measured regions. Marker/call instructions and warm-loop overhead remain
included; there is no estimated overhead subtraction.

Transfers strip-mine with the actual `vl`, using `e32,m1`. VLEN 256 moves eight
floats per chunk over a 32-byte/cycle link; VLEN 512 moves sixteen over a
64-byte/cycle link. Each MVM loads its input from SPM, executes array 0 once,
and stores its output. There is no matrix blocking, reprogramming, or partial-sum
accumulation inside the repeated batch.

The model uses its existing defaults: 100 cycles per array MVM, zero additional
programming settling cycles, 64-byte array buffering, and shared link direction.
SPM stays at 2 MiB, eight 32-bit (4-byte) banks, one read and one write port per bank, and two
32-byte/cycle channels. Instruction fetches use an 8 KiB, two-way cache with
64-byte lines, least recently used replacement, and one-cycle hits including
the CPU issue cycle. Misses fetch complete lines through the banked SPM.
Data accesses remain uncached. Contiguous RVV memory accesses use preauthorized
beats of at most VLEN/8 bytes, split at 32-byte SPM request boundaries. A full
VLEN-256 beat uses all eight banks in one service cycle; a VLEN-512 beat needs
two bank-service cycles. Larger VLEN reduces round trips and chunk/control
overhead while the SPM's aggregate bank bandwidth stays fixed.

With `--pipeline`, the first MVM is fully drained before measurement of the
repeated batch. The batch starts job 0, then prepares/starts job n before
draining job n−1, and finally drains the last result. Each array keeps a
protected input snapshot and two result slots; the next `mvm` waits for the
single compute engine. Computation latency remains 100 cycles. Input preparation
and output handling still share the CPU and register link, so this mode does
not imply three independent transfer engines or concurrent memory instructions.
Fragments within one RVV memory beat can progress concurrently through SPM.
The phase CSV includes actual input-transfer/compute and output-transfer/compute
overlap cycles. CPU input preparation can also overlap before a register-to-array
transfer starts. The checker verifies that output handling overlaps computation
in cached pipeline batches and that no two computations overlap on the single
array. Uncached instruction fetches may consume the available overlap window.

Instruction-cache hits, misses, fills, fill bytes, evictions, invalidations, and
stall cycles are included in the phase CSV. `instruction_bytes` counts logical
instruction bytes; `fetch_bytes` counts actual SPM instruction traffic, including
line fills. Warm MVMs reuse cached function code; entering the batch-loop code
for the first time can still cause a compulsory miss. These misses remain in
the measured time and are reported without extra untimed executions.

The harness validates every numerical output, exact analog command/byte counts,
resident weight reuse, per-array compute latency, link bandwidth, complete CPU
memory requests, instruction-cache counters and fill bytes, and the actual SST
topology (only CPU-to-array command links).
`report.md` contains the comparison, `summary.csv` the headline timings,
`phases.csv` the counter breakdowns, and `results.json` the complete statistics.
`metadata.json` records compiler commands/version, ELF/QEMU hashes, source
hashes, and the component build. Raw traces, logs, output vectors, and topology
are retained for each case. Host wall time is separate from simulated cycles.
