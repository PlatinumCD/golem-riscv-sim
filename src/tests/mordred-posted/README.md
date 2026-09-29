# Posted router writes

This fixture tests finite receiver reservations and source completion at NIC
ownership, with destination-local SPM completion notifications. It runs two
router/SPM interfaces without CPUs or analog arrays. Bulk writes into a slow
single bank force receiver-credit backpressure; sparse writes check advertised
physical-bank permissions.

From the repository root, build an isolated test plugin and run:

```sh
python3 -B - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, str(Path('src').resolve()))
from build import build
build('build/src-mordred-posted-test',
      extra_sources=['src/tests/mordred-posted/fixturedriver.cc'])
PY
python3 -B src/tests/mordred-posted/run.py \
  --build-info build/src-mordred-posted-test/build.json
```

The five configurations cover receiver capacity1/2, aggregation and final
partial credit returns, disabled receives, and a missing destination consumer.
Each checks exact backing bytes, physical bank fragments, packet/flit counts,
the source acceptance and destination commit timeline, and no posted network
completion responses. Invalid requests include a range whose permitted prefix
ends in a forbidden bank; the full-image oracle detects partial mutation.
The source caller releases its simulation hold while a final accepted write
remains in flight, testing endpoint delivery and credit-drain lifetime.

The [targeted comparison report](../../../tests/results/mordred-posted-comparison/report.md)
also records legacy compatibility, simultaneous posted/legacy ID reuse, and
two-tile MVM checks with unchanged guest binaries.
