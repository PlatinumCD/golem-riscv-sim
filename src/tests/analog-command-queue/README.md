# Analog command queue tests

These standalone tests compile their guests from repository sources and run the
real CPU, banked SPM, and two arrays. They do not require archived experiments or
saved study results. The queue validator in `validate.py` has no simulator or
experiment import dependency.

After building the model, run from the repository root:

```sh
python3 -B src/tests/analog-command-queue/run.py
python3 -B src/tests/analog-command-queue/admission.py
```

Both runners accept `--build-info`, `--qemu`, `--compiler`, and `--output`. Defaults
select `build/src-analog-queue/build.json`,
`build/src/qemu/qemu-system-riscv64`, and
`install/llvm/bin/clang`. Results default to fresh timestamped directories under
`tests/results/`. Output directories must be new or empty.

To include the previous synchronous model as an explicit historical baseline:

```sh
python3 -B src/tests/analog-command-queue/run.py \
  --baseline-build-info build/src-selective-lsq/build.json \
  --baseline-qemu build/src-qemu-selective-lsq/qemu-system-riscv64
python3 -B src/tests/analog-command-queue/admission.py \
  --baseline-build-info build/src-selective-lsq/build.json \
  --baseline-qemu build/src-qemu-selective-lsq/qemu-system-riscv64
```

Both baseline arguments must be supplied together. Candidate build input hashes
must match the current sources. Historical baseline source hashes may describe an
older revision; its build manifest and actual binary hashes are retained in run
metadata. The runners do not rebuild or replace either model.

`run.py` runs eight candidate cases, or nine with the optional baseline. The
sixteen-phase guest covers source WAR pins; output RAW/WAW hazards against RVV
ALU and LSQ operations; captured older stores; full/fractional LMUL and short-VL
tails; completion after a SEW/LMUL change; queue and byte pressure; two-array
result routing/FIFO order; high64-bit and backend-invalid array faults; VL0;
and fence, marker and exit drains. Configurations include VLEN256/1024, queue
depths0/1/4/8, LSQ depths1/16, and1024/16384-byte queue limits.

Programming delay is256 cycles so the source-capture assertion proves that
register reuse precedes whole-command completion. Exact numerical outputs and
untouched tails are checked for every run. Queue checks reconstruct occupancy,
slot identity, byte credits, physical delivery, completion, and every dependency
wait release. Byte-pressure cases must visibly reach their intended bounds.

With a baseline, disabled-queue timing and complete SPM contents must match.
Changing only the host instruction budget from256 to1 must preserve all modeled
trace events/timestamps after bijective backend-token renumbering, all phase/end
cycles, and full SPM contents. `--check-only --output PATH` revalidates saved
directed cases; `--compile-only` checks both guest builds without a simulator.

`admission.py` runs two candidate cases, or three with a baseline. A depth16 queue
and the same directed guest force backend Busy retries beyond the eight accepted
array entries. A separate no-result-output guest exercises blocking
Accepted→Error and a precise trap while an older captured Program remains in its
programming delay. The optional baseline must match the full SPM and architectural
work. These admission cases complement the original nine-case matrix.

[Validation and measured results](RESULTS.md) records the matched experiment and promotion checks.
