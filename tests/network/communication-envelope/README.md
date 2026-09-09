# Communication envelope regression

Hardware guests and measurement checks use tracked `tests/support`, `src/platform`, and
`tools/hardware/hardware_paths.py` infrastructure.

The nine regression cases cover delayed receive, fan-out, shared-link traffic,
duplex traffic, concurrent RVV/TX/RX, two bank placements, descriptor tails,
and bounded queue/FIFO backpressure. Guests check descriptor identity, order,
size and boundary data, final receive buffers, and RVV results. Analysis checks
byte conservation, completion counts, timestamp ordering, and service accounting.
Passing these checks does not establish peak client rates.

```bash
# Host checks only; no hardware tools needed.
bash tests/network/communication-envelope/run-test.sh --host-only
# Host checks followed by all nine integrated cases.
bash tests/run-all.sh --case network/communication-envelope
# Direct selection and per-case timeout.
bash tests/network/communication-envelope/run-test.sh --case duplex-t2 --timeout 90
python3 -B tests/network/communication-envelope/run.py --regression --list
```

Integrated execution requires the Golem LLVM compiler, QEMU, SST Core, and
Mittens installations resolved by `tools/hardware/hardware_paths.py`.
`GOLEM_LLVM_DIR`, `GOLEM_BUILD_ROOT`, and `GOLEM_INSTALL_ROOT` override defaults.
Artifacts default to `tests/results/communication-envelope/<timestamp>`.
When `GOLEM_BUILD_ROOT` is explicitly set (including the suite's unique per-case
artifact directory), artifacts use `$GOLEM_BUILD_ROOT/communication-envelope/<timestamp>`.
An explicit `--output` must remain inside that owner, or `tests/results` by default.
`run.json` records commands, hashes, and individual PASS/FAIL/TIMEOUT/BUILD_FAILED
statuses; per-case directories retain logs, traces, and analyzed `record.json`.

Optional cases cover client subsets, matched sharing/distance controls, load gaps,
and payload sizes through 64 KiB. They use the same local guest and analyzer;
they are not part of the nine-case regression. List all with
`python3 -B tests/network/communication-envelope/run.py --list`, then select
one with `--case NAME` or a category with `run.py --category CATEGORY`.
Host checks cover measurement availability, timing, case design, and prediction
training/holdout separation.
