# Instruction scheduling regression

Run through `bash tests/run-all.sh --suite hardware --case platform/instruction-issue`,
or run this directory's `run.py` with `--output`, matching `--qemu` and `--build-info`.

The native policy test checks RAW/WAW hazards, x0, integer/FP/vector register
decoding, LMUL/EMUL, widening/narrowing conversion groups, port conflicts,
pipelined result latency versus initiation interval, and serialization.

Eleven real QEMU/SST cases cover single/dual issue, restricted fetch and integer
ports, two memory ports, three-cycle ALU/vector timing, an occupied ALU, uncached
fetches and host budgets 1/7/256. All cases run the same guest and validate its
SPM results. Actual issue traces independently check total and per-unit width,
dependent issue spacing, and that a pointer chase cannot launch its next read
before the preceding read completes. The two smaller host budgets must produce
byte-identical instruction, memory and cache traces to the ordinary dual-issue run.

The warmed 128-instruction independent-add region must take 128 issue cycles
at width one and 64 at width two. Its dependent counterpart must take 128 in
both. Mixed scalar/RVV operations exercise distinct units under the same total
issue budget. Reported `kernel_issue_span` excludes setup, memory result stores
and markers; `task_cycles` includes that surrounding work.

`results.json` contains timings, configuration and functional checks;
`metadata.json` records the guest build, QEMU hash and component build manifest.
These are directed correctness/performance checks, not a parameter study.

See [validated results](RESULTS.md) for the single/dual comparison and the
compiler-generated convolution + ReLU replay.
