# Hardware tests

Run from the repository root. The explicit hardware registry is
[`tools/hardware/hardware_suite.py`](../tools/hardware/hardware_suite.py).
The hardware suite is independent of Sculptor runtime integration.

## Start with the default architecture

The machine now defaults to executable SPM and an 8 KiB instruction cache.
These checks target that path:

```bash
bash tests/run-all.sh --case configuration --case network/mesh-3x3 \
  --case platform/scratchpad-icache --case platform/icache-working-set \
  --case memory/spm-chunking --case memory/spm-code-capacity
```

- `configuration` checks defaults and rejects incompatible settings.
- `mesh-3x3` links its guests for SPM, checks two active endpoints and their routes.
- `scratchpad-icache` checks fetches, faults, modified code and RVV transaction width.
- `icache-working-set` checks correct execution below and above cache capacity.
- `spm-chunking` checks datasets larger than SPM through explicit DMA chunks.
- `spm-code-capacity` checks valid layouts and rejection of stack overlap.

All managed hardware guests target executable SPM. `platform/hello` runs through
SST, including boot DMA and instruction-cache timing. Numerical and payload
checks remain separate from performance counters; use the run's `results.json`
for actual pass/fail status.

## Full registry

```bash
bash tests/run-all.sh --list            # Registered hardware cases
bash tests/run-all.sh --suite hardware  # Full hardware suite
bash tests/run-all.sh --suite runtime --list  # Optional runtime integration
bash tests/run-all.sh --help
```

| Coverage | Intended checks | Command |
|---|---|---|
| Platform | Do boot, scratchpad-backed instruction caching, vector execution, CPU timing, and register/RVV accounting work? | `bash tests/run-all.sh --group platform` |
| Memory | Do global RAM, scratchpad DMA, oversized-data chunking, exact accounting, and DMA contention behave correctly? | `bash tests/run-all.sh --group memory` |
| Network | Do pair/mesh transfers, timing, fan-out, pipelines, and communication-envelope accounting work? | `bash tests/run-all.sh --group network` |
| Analog | Do instructions, operations, timing, routing, and distributed MVM/recombination work? | `bash tests/run-all.sh --group analog` |
| Runtime (optional) | Do the library, pair deployment, epoch barrier, runtime transmit fan-out, and distributed matvec work? | `bash tests/run-all.sh --suite runtime` |
| Host | Do hardware infrastructure checks pass? ([source](../tools/hardware/verify.py)) | `bash tests/run-all.sh --case host` |
| Configuration | Are component configurations validated? ([source](../src/sst/tests/run-configuration-test.py)) | `bash tests/run-all.sh --case configuration` |
| Components/controllers | Do isolated components, TX regressions, and software-owned RX payload checks pass? ([sources](../src/sst/tests)) | `bash tests/run-all.sh --case component --case tx-controller --case rx-controller` |
| Profile validation | Are performance profiles consistent? | `bash tests/run-all.sh --case validation/performance-profile` |

Memory-capacity checks include [oversized-data chunking](memory/spm-chunking/README.md),
[code/data capacity boundaries](memory/spm-code-capacity/README.md), and
[executed programs larger than the I-cache](platform/icache-working-set/README.md).

## Results and timing evidence

Suite evidence goes under `tests/results/test-runs/<timestamp>` with a
`results.json` manifest and per-case logs/artifacts. Integrated tests require
built hardware/toolchain installations; see the individual test READMEs.
The runner also checks which QEMU, SST element and guest binaries were loaded.
Use the overall manifest status, not only a guest's success printout.

Elapsed cycles, CPU instruction cycles, memory service and resource stalls are
different measurements. Do not add overlapping resource counters to produce
runtime. A submitted asynchronous DMA is not evidence that it overlapped CPU
work; check their timed intervals. Missing counters are not measured zeros.

## Optional runtime integration

`network/transmit-fanout` and `analog/distributed-matvec` belong to the optional
runtime suite; select them with `--suite runtime --case NAME`.

Receive-DMA host coverage remains in the component tests; the communication
envelope also checks RX behavior. Default `rx-controller` covers software-owned
payload reception without the external runtime. The receive-order and
head-blocking guests instantiate `DeploymentRuntime` and are retained separately:

```bash
GOLEM_SCULPTOR_SOURCE=/path/to/sculptor-checkout \
  bash tests/run-all.sh --suite runtime --case runtime/rx-controller
```

## Communication envelope

`network/communication-envelope` covers delayed receive, fan-out, shared links,
duplex traffic, RVV/DMA concurrency, bank placement, descriptor tails, and
bounded backpressure. It includes host measurement/design checks and nine
integrated hardware cases, with no dependency on local study files.
All nine cases execute from SPM with instruction-cache timing and use the
same bank arbitration for CPU, TX and RX accesses.
Direct runs default to `tests/results/communication-envelope/<timestamp>`;
suite runs respect the runner's unique `GOLEM_BUILD_ROOT` artifact directory.

```bash
bash tests/network/communication-envelope/run-test.sh --host-only
bash tests/run-all.sh --case network/communication-envelope
```

See [the case README](network/communication-envelope/README.md) for prerequisites,
direct case selection, output locations, and evidence limitations.
