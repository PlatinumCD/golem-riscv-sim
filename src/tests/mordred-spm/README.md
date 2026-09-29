# Four tiles sharing physical SPM banks with Mordred

```sh
python3 -B src/tests/mordred-spm/test_configuration.py
python3 -B src/tests/mordred-spm/run.py
```

The default configuration has four physical banks per tile. The CPU connects
to banks 0–3; the router endpoint connects to banks 2–3. Their requests access
the same bytes, bank ports, channel capacity, controller ordering and backing.
Arrays receive data exclusively through CPU vector registers.

Four real QEMU guests use RVV stores to initialize FP32 data in router-visible
bank stripes. Four **test-only** `SharedBankInitiator` components submit explicit
remote reads and writes through each endpoint's `requests` port. Each client
reads the next tile's CPU-written values, writes a different known input, and
waits for responses before publishing a test flag. Each guest uses RVV loads to
verify its received input and executes a 32×32 identity MVM using that input.
The CPU currently accesses local SPM; these fixtures do not imply a CPU ISA or
address mapping for issuing remote requests. Production endpoints do not poll
flags or execute guest commands.

| Case | CPU banks | Router banks | Bank width | Extra coverage |
|---|---|---|---:|---|
| `shared4` | 0–3 | 2, 3 | 4 B | Shared backing and arbitration |
| `remapped` | 0–3 | 0, 2 | 4 B | Nonadjacent physical bank mapping |
| `width8` | 0–3 | 2, 3 | 8 B | Wider physical banks |
| `request4` | 0–3 | 2, 3 | 8 B | 4 B request fragments, two memory credits |
| `vlen512` | 0–3 | 2, 3 | 4 B | VLEN 512; default is 256 |
| `budget1` | 0–3 | 2, 3 | 4 B | Same timing and traces with QEMU budget 1 |
| `cpu-denied-fetch` | 2, 3 | 2, 3 | 4 B | Actual CPU instruction fetch rejected at bank 0 |

Positive cases use CPU LSQ depth 8 and ASQ depth 4, one array and a 2 MiB SPM
per tile. Each test client uses four concurrent requests and deliberately
exceeds its window once. Forbidden reads/writes, mixed allowed/forbidden spans,
out-of-range requests and malformed requests must reject without memory work.
The guest and independent host oracle verify all private probe bytes remain
unchanged. The final SPM oracle compares every nonstack byte against the ELF
and independently constructed expected data, including unused bank stripes.

Validation checks topology, every bank/channel service, exact bank connection
sets, source visibility before remote reads, remote write completion before
CPU reads, and all network request/response/memory timing chains. It checks
finite windows, final endpoint quiescence, actual flit-rounded packet bytes,
and simultaneous pending CPU/router activity. The budget comparison preserves
all bank events/timestamps with request IDs normalized bijectively and compares
client event traces exactly. This suite measures correctness, not throughput.

Results go under `tests/results/source-new-mordred-spm/<run>/`. Use `--case
shared4` to select a case, `--output` for a fresh result directory, and
`--build-dir` to select an isolated build. The builder includes only the
test-only `initiator.cc` as an extra fixture. `--reuse-build` requires matching
compiled source fingerprints and an existing build containing that fixture.
QEMU defaults to `build/src/qemu/qemu-system-riscv64`;
`--qemu` and `--compiler` select other compatible local binaries.
