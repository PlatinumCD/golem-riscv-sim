# Exact byte-range ordering regressions

From the repository root, build the test plugin with `protocol.cc`, then run:

```sh
python3 -B - <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path('src').resolve()))
from build import build
build(Path('build/src'), with_tests=True,
      extra_sources=[Path('src/tests/range-ordering/protocol.cc')])
PY
python3 -B src/tests/range-ordering/run.py --build-info build/src/components/build.json
```

An existing shared test build can be reused if it includes this fixture and its
recorded source hashes still match. Other suites may require their own extra
fixture sources in the same build.

Sixteen cases use a controlled external CPU interface and an independent
StandardMem peer. The CPU interface delays its functional mmap access and
`ExternalCommit` for twenty cycles after the timing response. This makes the
otherwise short response-to-commit ordering window directly observable.

The eleven successful cases check simultaneous service of disjoint 16-byte
halves of one 32-byte request line, both between CPU requests and between CPU
and peer; exact and partial RAW/WAR/WAW overlap; preservation of an older queued
overlapping request when a younger request is disjoint from the active request;
and a 64-byte transfer's two 32-byte fragments released separately or as a batch.
Backend traces check admission, bank ports, channels, completion and commit
boundaries. Returned data and the complete 4 KiB backing image are checked.

Five negative cases require controller rejection of empty, duplicate, premature,
wrong-size and foreign-peer commits. Backing data must remain unchanged, including
when a peer write waits behind a completed external read. Negative cases must
fail through the controller's commit validation, rather than a fixture timeout.

The direct fixture isolates controller ordering. The existing 21-case
load/store-queue suite additionally checks real QEMU functional commits,
overlapping memory dependencies, peer observations, and wide RVV accesses.
