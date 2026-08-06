# CPU timing validation — 2026-07-30

## Result

PASS. All 18 predicted-versus-measured region checks have zero-cycle error,
and changing QEMU's host synchronization quantum from 37 to 1,000
instructions does not change any architectural result.

## Configuration

- CPU clock: 1 GHz
- scalar issue widths: 1, 2, and 4 instructions/cycle
- vector issue limit: one RVV instruction/cycle
- RVV geometry: VLEN 256, ELEN 64
- test bodies: 1,024 scalar `addi` instructions and 1,027 RVV instructions
- synchronization quanta: 37 and 1,000 instructions

## Exact observations

| Issue width | Region | Retired instructions | RVV instructions | Predicted cycles | Measured cycles |
|---:|---|---:|---:|---:|---:|
| 1 | baseline | 6 | 0 | 6 | 6 |
| 1 | scalar | 1,030 | 0 | 1,030 | 1,030 |
| 1 | RVV | 1,035 | 1,027 | 1,035 | 1,035 |
| 2 | baseline | 6 | 0 | 3 | 3 |
| 2 | scalar | 1,030 | 0 | 515 | 515 |
| 2 | RVV | 1,035 | 1,027 | 1,027 | 1,027 |
| 4 | baseline | 6 | 0 | 2 | 2 |
| 4 | scalar | 1,030 | 0 | 258 | 258 |
| 4 | RVV | 1,035 | 1,027 | 1,027 | 1,027 |

Every row was observed identically at both synchronization quanta. Complete
machine-readable output is generated at
`build/tests/cpu-timing-validation/results.csv`.

## Corrections exposed by the gate

The first validation run found two timing defects:

1. QEMU's generic post-decode counter omitted `vsetivli` because that
   instruction exits its translation block during decode.
2. SST rounded issue-width occupancy independently at every QEMU quantum,
   making width-2 and width-4 completion depend on a host batching parameter.

QEMU now counts vector-set instructions at their decode sites. Mittens now
carries partial scalar/vector issue occupancy across `QUANTUM_END` yields and
resets it only at a real architectural fd-41 boundary. The validation then
passed exactly.

Run:

```bash
./tests/cpu-timing-validation/run-test.sh
```
