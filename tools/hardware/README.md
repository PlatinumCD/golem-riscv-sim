# Current simulator tooling

The maintained source is [`src`](../../src/README.md). `source_new` resolves to
that same directory for existing study scripts.

```sh
JOBS=8 bash bootstrap.sh build-hardware
python3 -B tools/hardware/verify.py
bash tests/run-all.sh --list
bash tests/run-all.sh --suite hardware
bash tests/run-all.sh --case network/posted-transfers
bash tests/run-all.sh --suite compiler
```

The builder creates `build/src/components/{libtilecomponents.so,libmordred.so}`
and `build/src/qemu/qemu-system-riscv64`, then installs them under `install/src`.
`current-build.json` records installed artifact hashes and individual build
manifests. Override `GOLEM_BUILD_ROOT` and `GOLEM_INSTALL_ROOT` for an isolated
build. The shared dependencies remain under `install/{sst-core,sst-elements,llvm}`.
`tools/hardware/env.sh COMMAND ...` selects the current installation explicitly.
Use `tools/hardware/env.sh --profile OUTPUT_DIRECTORY COMMAND ...` to enable
cycle profiling, or `--no-profile` to disable an inherited setting. Profiling is
off by default. The correctness runner accepts `--profile` and puts profiles
beside each simulation's test evidence. See [profiling](../../docs/profiling.md).

The test runner compiles its SST fixture components once per run. Each suite
retains its numerical and timing validators and writes separate evidence under
`tests/results/hardware/<run>/`. The top-level `results.json` records commands,
statuses, timings, logs and build identity. `--build-info` can reuse a matching
fixture build; stale inputs and missing fixtures are rejected. `--output` must
name a new directory. `--timeout` applies to each suite and terminates its process
group on expiry.

`verify.py` runs configuration/wiring and tooling checks without QEMU/SST.
`check-build.py` verifies installed identities and executes the CPU regression.
The compiler suite additionally requires an installed Sculptor compiler.
Historical model comparisons are retired; see [migration and recovery](../../docs/migration.md).
