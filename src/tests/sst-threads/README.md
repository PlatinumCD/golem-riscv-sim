# One simulation across SST workers

This correctness suite uses the normal `connect_riscv_mesh` placement. It
compiles each guest once and compares the same program on one and multiple
SST workers. It is independent of the temporary performance sweep.

```sh
python3 -B src/tests/sst-threads/test_configuration.py
python3 -B src/tests/sst-threads/run.py --threads 1 2 4 8 16 \
  --output /tmp/golem-thread-check
```

Use `--build-info /path/to/build.json` to reuse a current component build.
The default thread list is `1 2 4`; counts larger than a case's tile count are
skipped. The 4×4 case supports the 8- and 16-thread checks. Select a single
workload with `--case`, or provide `--qemu` and `--compiler` explicitly.

Coverage includes:

- MVM → multi-hop message → MVM on 2×2 and 4×4 compute meshes.
- Independent senders with reserved receive slots at one destination.
- Source reuse before a delayed receiver consumes the message.
- A DRAM tile programming two compute tiles, with single-slot backpressure.
- Full-run profiling enabled and disabled at each case's largest worker count.
- Rejection of missing CPU placement and a process-global task trace gate.

Every successful run invokes the existing network or DRAM numerical and
ownership validator. Complete component statistics, simulated completion time,
and complete SPM image hashes must match the one-thread reference. NIU message
traces also match after the existing validator normalizes command event IDs.
The exported SST topology must place all tile-local components together; only
the existing 1 ns router links may cross threads. Statistics must be valid JSON
and all CPU queues must drain. Each run preserves commands, guest hashes,
topology, traces and validation results under the selected output directory.

The main hardware suite includes this as `network/sst-threads`. See
[threading controls and limitations](../../../docs/threading.md).
