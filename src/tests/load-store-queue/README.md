# Load/store queue regressions

Run `python3 -B src/tests/load-store-queue/run.py --build-info build/src/components/build.json`
after building the isolated QEMU and a shared SST plugin containing
`tests/riscv-qemu/peer.cc`. The runner verifies build hashes and reuses that
plugin; it never rebuilds simulator components.

Twenty-five cases exercise queue depths 1, 2, 4, 8 and 16, VLEN 128/256/512/1024, one-bank
queue pressure, eight-bank split requests, and instruction budgets/issue widths.
Depth 1 is the legacy synchronous reference. Larger depths must overlap
independent accesses, respect capacity, and retire entries in order. The repeated
depth-4 case and one-instruction grants must reproduce the same LSQ trace and
phase timing.

The main guest checks every value from 24 independent register loads/stores;
load-to-store and arithmetic dependencies; two loads writing one register;
a store whose source register is subsequently overwritten by an independent
load or a vector-immediate move; and identical or partially overlapping memory
accesses in both directions.
A separate independent StandardMem peer polls vector output around functional
commit and verifies that the CPU observes peer writes to data and code.

Dedicated cases issue pending stores before `fence`, `fence.i`, a successful
guest exit, and a precise out-of-SPM load fault. Task markers have no guest
fences, so completion snapshots also check the marker drain. An analog-array
case checks pending loads before `mvm.vset`, `mvm.vl`, and `mvm.vs`, and stores
whose source is overwritten by a subsequent analog result. The existing
vector-memory guest is reused at depth 4 for masks, misalignment, nonzero vstart,
whole-register transfers, protected/faulting suffixes, and code modification.

Four boundary-VLEN cases compare short EEW 8/16/32/64 loads and stores at depths
1 and 4, with both tail policies. They check active bytes, retained store tails,
full destination registers, agreement with synchronous QEMU tail behavior,
and exact async tokens for every short transfer. A separate instruction-fetch
fault jumps to unmapped memory while a load and store remain outstanding.
Validation requires both to retire before the first trap-handler fetch, then
checks the register and memory values observed by the handler.

Four grouped-transfer cases run VLEN 128 and 1024 at depths 1 and 4. Each
contains 51 measured phases: whole-register loads/stores of 1/2/4/8 registers
with load EEW 8/16/32/64, VL zero and legal or illegal vtype; ordinary LMUL
2/4/8 partial transfers of every EEW and both tail policies; and VL one with
wholly inactive registers. Upper-register RAW/WAW dependencies, captured store
payloads, a pending load followed by a vtype change and tail-only register
write, and grouped arithmetic all have independent byte oracles. Cross-page
whole-register accesses and nonzero-vstart whole/ordinary transfers must retain
synchronous fallback. The trace oracle requires exact active-byte beats no
larger than VLEN/8, including groups larger than queue capacity. Full-register
spills must agree with synchronous QEMU's tail policy.

Run only those four new cases, without any study sweep:

```sh
python3 -B src/tests/load-store-queue/run.py \
  --case grouped-vlen128-depth-1 --case grouped-vlen128-depth-4 \
  --case grouped-vlen1024-depth-1 --case grouped-vlen1024-depth-4
```

The tokenized LSQ trace is the queue lifecycle oracle. Validation checks each
enqueue, issue, backend completion and functional completion, slot reuse,
occupancy, retirement order, and stall counters. The final SPM image supplies
the functional oracle. CSV traces, ELF disassembly and source/build identities
are retained under `tests/results/source-new-load-store-queue/`.
