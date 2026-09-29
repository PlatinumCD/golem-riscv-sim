# Instruction-cache integration tests

Run `python3 -B src/tests/instruction-cache/run.py` from the repository
root. This builds a small LLVM bare-metal guest and the SST component, then runs
the isolated QEMU against the shared banked SPM. `--build-info PATH/build.json`
can reuse a component after checking its recorded C++ source/header hashes.
`--case NAME` selects individual cases; `--output PATH` selects the artifact
directory. The default is `tests/results/source-new-instruction-cache/<time>/`.
Before SST runs, a standalone C++17 policy test checks LRU replacement against
a FIFO counterexample, invalidation, crossing fetches, and rejected geometry
and addresses. It uses the real cache class without SST dependencies.

The guest records four task phases: a 128-iteration loop within one cache line,
a 32-iteration loop through three separate lines, a 32-bit instruction starting
at byte 62 of a 64-byte line, and self-modifying code returning 17 before a
patch and 29 after `fence.i`. A second instruction starts at byte 2 to cross
four-byte cache lines within one SPM ordering line. The host checks all six
results directly in SPM.
Assembly symbol checks ensure that the intended alignments survive linking.

Ten configurations cover the default 8 KiB, 64-byte, two-way cache; a 128-byte
two-way cache with conflict evictions; an uncached reference; four-cycle cache
hits; instruction budgets of one and seven; issue width four; four-byte lines
in a 64-byte cache; four-byte SPM requests; and one-byte-per-cycle SPM channels.
The small cache-line case fills both halves of a crossing instruction within
one 32-byte SPM ordering line. The harness
checks cache and task counters, actual refill traces, both boundary-line fills,
post-`fence.i` refills, and per-request bank/channel service. Cross-case checks
cover eviction, fetch traffic, hit latency, request splitting, and SPM latency.

`fetch_bytes` counts actual SPM instruction traffic; `instruction_bytes` counts
logical instruction bytes. Cached traffic must equal `icache_fill_bytes`.
Cache hits use no SPM requests. The self-modification check covers both QEMU's
functional translation invalidation and the modeled SST cache invalidation.
