# Analysis tools

Python command-line tools for SST progress, deadlock and performance analysis,
residency feasibility, model-suite reports, artifact extraction, partitioning,
output comparison, and checkpoint timeout supervision. Run any tool with
`python3 tools/analysis/<name>.py --help` for its inputs and outputs.

Compiler artifact qualification lives in `../compiler/`. Regression fixtures
live under `tests/validation/`; run their `run-test.sh` entry points. The SST
partition check is `python3 tests/validation/sst-partitioning/test-validator.py`.
