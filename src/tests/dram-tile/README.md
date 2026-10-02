# DRAM Tile validation

See the [component contract and parameters](../../components/dram_tile/README.md).

```sh
python3 -B src/tests/dram-tile/test_configuration.py
python3 -B src/tests/dram-tile/run.py --output /tmp/golem-dram-tile-check
```

To reuse a current component build, add `--build-info /path/to/build.json`.
Use `--case weights-two-destinations` for the smallest check. The full six-case
suite checks real DRAM reads, mesh transport, direct SPM receive ownership,
RVV array programming and numerical MVM results on two destination tiles.
It uses the default two DRAM channels and one SPM write port per bank, with
an explicit one-channel case to check configuration overrides.

Each run writes its guest ELFs, commands, topology, simulation log, backing
files, traces and `validation.json`. The top-level `results.json` includes
DRAM request/byte counts, outstanding-read peaks and guest completion cycles.
The timing-change case is a sensitivity check, not a DRAM bandwidth benchmark.
