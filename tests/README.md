# Hardware tests

Run from the repository root. The explicit hardware registry is
[`tools/hardware/hardware_suite.py`](../tools/hardware/hardware_suite.py).
The default hardware suite is independent of Sculptor runtime integration.

```bash
bash tests/run-all.sh --list            # Registered hardware cases
bash tests/run-all.sh --suite hardware  # Full hardware suite
bash tests/run-all.sh --suite runtime --list  # Optional runtime integration
bash tests/run-all.sh --help
```

| Coverage | Question checked | Command |
|---|---|---|
| Platform | Do boot, vector execution, CPU timing, and register/RVV accounting work? | `bash tests/run-all.sh --group platform` |
| Memory | Do global RAM, scratchpad DMA, exact accounting, and DMA contention behave correctly? | `bash tests/run-all.sh --group memory` |
| Network | Do pair/mesh transfers, timing, fan-out, pipelines, and communication-envelope accounting work? | `bash tests/run-all.sh --group network` |
| Analog | Do instructions, operations, timing, routing, and distributed MVM/recombination work? | `bash tests/run-all.sh --group analog` |
| Runtime (optional) | Do the library, pair deployment, epoch barrier, runtime transmit fan-out, and distributed matvec work? | `bash tests/run-all.sh --suite runtime` |
| Host | Do hardware infrastructure checks pass? ([source](../tools/hardware/verify.py)) | `bash tests/run-all.sh --case host` |
| Configuration | Are component configurations validated? ([source](../src/sst/tests/run-configuration-test.py)) | `bash tests/run-all.sh --case configuration` |
| Components/controllers | Do isolated components, TX regressions, and software-owned RX payload checks pass? ([sources](../src/sst/tests)) | `bash tests/run-all.sh --case component --case tx-controller --case rx-controller` |
| Profile validation | Are performance profiles consistent? | `bash tests/run-all.sh --case validation/performance-profile` |

Suite evidence goes under `tests/results/test-runs/<timestamp>` with a
`results.json` manifest and per-case logs/artifacts. Integrated tests require
built hardware/toolchain installations; see the individual test READMEs.
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
Direct runs default to `tests/results/communication-envelope/<timestamp>`;
suite runs respect the runner's unique `GOLEM_BUILD_ROOT` artifact directory.

```bash
bash tests/network/communication-envelope/run-test.sh --host-only
bash tests/run-all.sh --case network/communication-envelope
```

See [the case README](network/communication-envelope/README.md) for prerequisites,
direct case selection, output locations, and evidence limitations.
