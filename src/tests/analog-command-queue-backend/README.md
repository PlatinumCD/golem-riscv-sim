# Analog command backend regressions

Run from the repository root:

```sh
python3 -B src/tests/analog-command-queue-backend/run.py
```

The runner compiles the current array backend into an isolated SST plugin and
runs eight cases: each of four drivers with deferred commands disabled and
enabled. It needs SST but does not need QEMU or the LLVM guest compiler. Outputs
go under `tests/results/source-new-analog-command-queue-backend/`; `--output`
chooses a new directory, `--sst-core` selects the SST installation, and
`--array-source` selects a compatible array backend directory. All runs are
sequential and no shared model binaries are rebuilt.

- **PipelineProtocol:** queued duplicate/partial stores, two result reservations,
  input snapshots, and stores bypassing a blocked third computation.
- **EpochProbe:** overlapping programming ranges, four-slot backpressure, and
  projected epoch closure rejection before a guaranteed command can fail later.
- **NonpipelineProbe:** guaranteed output after an active valid compute, and
  blocking Accepted-to-Error fallback when an earlier input load invalidates it.
- **CaptureProbe:** two simultaneous 256-byte source transfers, an aggregate
  16-byte/cycle link bound, capture on final timed byte release, seven-cycle
  programming delay after capture, and empty load/store transfers.

The C++ drivers assert admission/response order, capture semantics, result values,
compute latency, and clean completion. Python also checks global multi-array
input bandwidth and joins capture responses to actual byte-release traces.
Each case retains its configuration, topology, traces and validation. The final
`validation.json` records all eight results and source/plugin hashes. This suite
uses the current backend in both modes; it contains no historical source copy.
