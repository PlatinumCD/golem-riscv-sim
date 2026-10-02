# Physical bank connections

This fixture connects ordinary CPU and router memory interfaces to one SPM:
four physical 4-byte banks, CPU connections `[0, 2]`, router connections
`[1, 2]`. Bank 2 is the same stored data and the same port resources for both.

It checks CPU-write/router-read and router-write/CPU-read visibility, concurrent
service on independent banks, serialization on a shared write port, parallel
service with two write ports, and independent read/write ports. Disconnected
bank accesses, requests spanning connected and disconnected banks, and unknown
requestors must fail before any backing bytes change.
Additional visibility cases vary the connected bank counts to four CPU/one
router bank and one CPU/three router banks.

Build `src` with `driver.cc` supplied in `extra_sources`, then run:

```sh
python3 -B src/tests/bank-connections/run.py \
  --build-info build/src-bank-connections/build.json \
  --output tests/results/source-new-bank-connections/physical-banks
```

The full four-tile QEMU and Mordred integration lives in
[guest network instructions](../network-instructions/README.md).
