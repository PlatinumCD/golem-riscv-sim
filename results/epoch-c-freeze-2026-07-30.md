# Epoch C freeze — 2026-07-30

Epoch C is the first component-validated baseline for architectural
experiments. Its frozen parameters are machine-readable in
`config/epoch-c.env`; third-party revisions are pinned in
`config/versions.env`. The project revision is the commit containing those
manifests.

## Component gates

| Boundary | Acceptance evidence | Result |
|---|---|---|
| CPU scalar/RVV issue timing | widths 1/2/4, quanta 37/1,000, exact counters and cycles | PASS |
| NIC/router timing | serialization, 1/2/4 hops, and 1/2/4-source contention; 21 packet observations | PASS |
| Analog timing | set/load/execute/store; shared-link contention and independent compute overlap | PASS |
| Private-L1 timing | exact hit/miss, conflict, LRU, capacity, and response latency | PASS |
| Transmit synchronization | blocking/async/polling policies and host-quantum invariance | PASS |
| Focused regressions | element tests, RVV execution, L1 integration, analog timing, network timing, fanout | PASS |

The standardized summary is
`results/epoch-c-component-validation.csv`. Detailed reports are:

- `results/cpu-timing-validation-2026-07-30.md`
- `results/network-timing-validation-2026-07-30.md`
- `results/analog-timing-validation-2026-07-30.md`
- `results/private-l1-timing-validation-2026-07-30.md`

## Scope

Epoch C validates the timing abstractions the planned native-memory GPT-2
scheduling experiment exercises. It does not claim a detailed out-of-order
CPU, vector pipeline, instruction cache, TLB, coherent CPU/DMA cache, DRAM,
analog device nonideality, or energy model. The optional private-L1 gate
qualifies controlled memory studies; the frozen GPT-2 sweep continues to use
the explicitly ideal `native` data-memory backend.

No GPT-2 scheduling sweep was run as part of this freeze. The next experiment
is a fresh sweep whose outputs identify Epoch C and preserve this manifest.

All component gates can be reproduced, without running that sweep, with:

```bash
./tests/epoch-c-validation/run-test.sh
```
