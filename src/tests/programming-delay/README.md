# Initial whole-array programming delay

Run `python3 -B src/tests/programming-delay/run.py`. The runner builds a
separate plugin at `build/src-programming-delay`; `--build-info` reuses a
matching build, and `--case` selects an individual fixture. Tests preserve the
existing QEMU binary and simulator installation.

The direct-command fixtures cover both pipeline modes, one and two arrays,
zero/nonzero delay, and both programming scopes. They verify missing coverage,
last-address writes, duplicate and overlapping coverage, already queued and
new reprogramming attempts, attempts during the delay, and zero-length no-ops.
MVM outputs verify that rejected writes preserve the completed weights.

Real LLVM/RVV guests cover 32×32 and 64×64 matrices, VLEN256/512 and LMUL1/2/4/8
in both pipeline modes. Each identical binary runs under default zero delay,
initial-full-array zero delay, and a 23-cycle initial delay. Controls require
identical CPU counters and array trace bytes between the zero-delay scopes,
exactly 23 added programming cycles, one delay charge independent of register
group size, and numerically correct results after timing.

Validation reconstructs unique coverage and links delay events to final-byte
transit and command completion. Invalid scope rejection is checked directly in
the C++ component as well as by Python configuration tests. The runner also
reuses the existing independent array regressions and their original oracles;
`--skip-legacy` omits those when selecting a focused case.
