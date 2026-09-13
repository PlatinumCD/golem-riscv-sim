# Programs larger than the I-cache

Execute **4, 8, 12 and 20 KiB code bodies** with a fixed 8 KiB I-cache,
32 KiB SPM and 4 KiB data window. The full executable, data, guards and
2 KiB stack must still fit SPM. The code body is straight-line arithmetic
whose return value is checked and used in every output—not unexecuted padding.

For each input chunk, submit a main-memory-to-SPM DMA, traverse the code body
three times, wait for DMA completion and transform the input. Copy results
back to main memory and verify every word. No window is reused before its
DMA completes. The dataset is 64 KiB + 12 bytes and includes a short tail.

The checks require:

- Correct kernel return values, all output words, guards and tail storage.
- Exact DMA bytes and completions, with boot/setup/verification separate.
- A cold first traversal that fills the code body into the cache.
- Much fewer fills on the second traversal of the 4 KiB body.
- Repeated refilling for the 12 and 20 KiB bodies, which exceed cache capacity.

DMA service bytes within each traversal are reported from timestamps, not
assumed from submission order. Zero means no overlap was observed; this test
does not require or demonstrate compute/DMA overlap merely by passing.

The 8 KiB body is a boundary case, not a promise of zero warm misses: startup,
task markers and surrounding control code also use the cache. Per-traversal
fill bytes come from instruction-fetch SPM beats within task intervals; they
may include marker/call overhead. Whole-run hit/miss counters are also retained.
This tests correctness and cache behavior, not a fitted performance model.

Run `bash tests/run-all.sh --case platform/icache-working-set`.
Fixture sources and analyzer live in `tests/memory/spm-code-capacity/`.
Results are isolated under `tests/results/test-runs/` by the normal runner.
