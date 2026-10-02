# 2×2 Mordred runtime tests

```sh
python3 -B src/tests/mordred/test_configuration.py
python3 -B src/tests/mordred/run.py
```

`run.py` builds the pinned network and a test-only `SimpleNetwork` endpoint into
`build/src-mordred-tests/`, then runs SST with four routers and four
endpoints. It does not run the CPU or any existing study. `--output` selects a
new result directory; `--build-dir` selects the local test library directory.

Each endpoint sends to all three remote endpoints. Packets identify their
source, destination, sequence, and virtual network, and carry deterministic
byte payloads. Endpoints verify every byte, reject duplicate/unexpected packets,
and prevent simulation exit until all sends and receives finish. A simulation
cycle timeout and wall timeout turn deadlock or loss into a test failure.
The runner also audits the actual SST graph and checks per-port packet/flit
statistics against the expected XY routes, including fragmentation rounding.

| Case | Packet bytes | Packets per directed pair | Purpose |
|---|---:|---:|---|
| `all-to-all` | 64 | 16 | All 12 directed routes and concurrent traffic |
| `minimum-packet` | 32 | 8 | Two-flit packet boundary |
| `partial-flit` | 33 | 8 | Nonintegral flit count and tail payload |
| `large-packet` | 1024 | 32 | Fragmentation beyond router buffer capacity |
| `tight-credits` | 256 | 64 | One-flit router buffers; one-packet NIC transmit buffer |
| `two-vcs` | 64 | 16 | Two router virtual channels, still one VN |
| `slower-links` | 64 | 16 | 3ns links delay first delivery compared with 1ns links |

Results are saved under `tests/results/source-new-mordred/<run>/`. Each case
contains `case.json`, `topology.json`, `statistics.csv`, `simulation.log`, four
endpoint JSON files, and `validation.json`. The top-level validation records
the pinned upstream revision, source and library hashes, and commands.

`send_blocked_cycles` measures endpoint cycles rejected by `spaceToSend()`;
it is a count of denied admission attempts, not a decomposition of router
latency. Packet latency starts at successful NIC admission and ends at endpoint
delivery notification; endpoint consumption latency is reported separately.
Overall completion time under contention need not increase monotonically with
link latency, since packet spacing changes arbitration. Upstream's completed
receive-packet queue is unbounded, so this suite
does not claim to validate backpressure from a stalled application receiver.

The fixture contains network endpoints rather than RISC-V guests. See the
[component README](../../components/mordred/README.md) for composition and the
[guest messaging tests](../network-instructions/README.md).
