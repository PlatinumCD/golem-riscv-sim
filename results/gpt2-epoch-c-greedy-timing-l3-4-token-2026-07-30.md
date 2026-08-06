# Epoch C GPT-2 analog/digital comparison — 2026-07-30

This is a focused two-run validation, not a rerun of the 112-case sweep.

## Configuration

- workload: GPT-2-small-shaped fixture, four static tokens
- scheduler: timing-aware Greedy
- lookahead: 3
- beam width: 1
- scope: diagonal
- backend-aware placement costs
- mesh: 8x8
- CPU: 1 GHz, scalar issue width 2, one vector instruction/cycle
- memory: native
- analog arrays: four 1024x512 arrays per tile
- mesh link: 32 bits/cycle

## Result

| Metric | Analog | Digital |
|---|---:|---:|
| Simulated time | 19.2439 ms | 120.132 ms |
| Active cores | 61 | 64 |
| Tasks | 2,521 | 2,521 |
| Routes | 1,227 | 1,306 |
| Inter-core bytes | 3,362,816 | 3,608,576 |
| Retired instructions | 53,509,616 | 329,597,322 |
| RVV instructions | 8,698,096 | 161,100,458 |
| Modeled CPU cycles | 26,757,022 | 173,220,981 |
| Transmitted 32-bit words | 846,839 | 908,674 |
| Analog-active cycles | 15,920,536 | 0 |
| Finite outputs | 3,072/3,072 | 3,072/3,072 |

The analog deployment is **6.2426x faster** in simulated end-to-end time,
an 83.98% reduction. The digital deployment retires 6.1596x more
instructions and accumulates 6.4739x more modeled CPU cycles. Its network
traffic is only 7.30% higher, so the dominant difference in this comparison is
digital compute work rather than communication volume.

This compares one scheduler policy with backend-specific timing costs, not one
fixed placement: 2,484 of 2,521 tasks (98.53%) receive different core
assignments. The comparison therefore captures the combined effect of backend
choice and the schedule that the same timing-aware policy selects for that
backend.

Both runs pass and produce 3,072 finite outputs. Their bit signatures differ,
so this smoke comparison does not by itself establish analog/digital numerical
equivalence or an accuracy bound; that requires a separate reference-output
comparison.

Machine-readable data is in
`results/gpt2-epoch-c-greedy-timing-l3-4-token-2026-07-30.csv`.
