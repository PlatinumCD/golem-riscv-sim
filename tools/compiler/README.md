# Compiler tools

Python command-line tools for architecture manifests, memory contracts,
materialization and residency audits, compile qualification, and reproducible
run manifests. Run `python3 tools/compiler/<name>.py --help` for usage.

Differential validators are named for the contract they check: placement,
physical plan, scalar regions, or dependencies.

Regression fixtures live in `tests/validation/`, including
`scalar-region-differential` and `scalar-region-suite-preflight`; invoke their
`run-test.sh` scripts. Building and lowering remain in `build-scripts/`.
