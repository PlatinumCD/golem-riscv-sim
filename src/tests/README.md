# Component regression suites

Run all maintained suites through `bash tests/run-all.sh --suite hardware` from
the repository root. Use `--list` or `--case NAME` to select coverage; see the
[top-level test guide](../../tests/README.md). Individual `run.py` scripts remain
available for directed work. The main runner supplies matching QEMU and a fresh
fixture build automatically.
