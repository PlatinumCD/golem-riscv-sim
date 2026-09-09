# Producer–MVM–recombine distance study

This controlled test measures how placement distance changes the latency of:

```text
producer -- 512-word activation --> two-array MVM
         <-- two 256-word partials -- recombine
```

The test uses a fixed 9x9 mesh, one-word 32-bit timing cells on 1 GHz mesh
links, 16,384-word (64 KiB) buffering, dual-issue 1 GHz RISC-V tiles, native
memory, two 256x512 analog arrays, one shared 256-bit 1 GHz analog link, and
100-cycle array execution. The MVM tile programs and warms both arrays
before releasing the producer. Consequently, the measured interval starts at
the producer task and excludes matrix programming.

Three experiment families are generated:

- `activation-distance`: move the producer 0, 1, 2, 4, or 8 hops from a
  colocated MVM and recombine stage.
- `partial-distance`: move recombination 0, 1, 2, 4, or 8 hops from a
  colocated producer and MVM stage.
- `combined-distance`: sweep both distances over `{0, 1, 2, 4, 8}`.

The activation path is horizontal. The partial-result path is vertical. This
keeps the two boundaries geometrically distinct and makes their Manhattan
distances exact.

Run the four-point acceptance subset:

```bash
tests/analog/producer-mvm-recombine-distance/run-test.sh
```

Run all 35 placements:

```bash
tests/analog/producer-mvm-recombine-distance/run-sweep.sh
```

Outputs are under:

```text
tests/results/producer-mvm-recombine-distance/
├── configurations.tsv
├── configurations/<name>/tile-*.elf
└── runs/
    ├── quick/
    │   ├── results.csv
    │   └── <name>/{profile,tasks,simulation.log}
    └── full/
        ├── results.csv
        └── <name>/{profile,tasks,simulation.log}
```

`results.csv` reports total task-graph latency, producer time, activation
boundary delay, MVM time, partial boundary delay, recombination time, analog
service time, task instructions, payload and protocol traffic, word-hops,
endpoint queue time, and receive-DMA cycles.
