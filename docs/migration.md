# Migration to the component simulator

`src/` is the maintained implementation. `source_new` is a relative compatibility
symlink to `src`; it contains no second implementation. Existing study scripts can
continue using that path. New code and documentation should use `src`.

The previous implementation and its retired tests remain available in Git
history before this migration. The component release contains the maintained
model and correctness fixtures; studies and dashboard changes are excluded.

## Maintained entry points

```sh
bash bootstrap.sh check
JOBS=8 bash bootstrap.sh build-hardware
python3 -B tools/hardware/verify.py
bash tests/run-all.sh --list
bash tests/run-all.sh --suite hardware
bash tests/run-all.sh --suite compiler
```

`build-hardware` uses the installed shared SST/compiler dependencies and builds
both current SST libraries and the matching QEMU. `bootstrap.sh build` also builds
shared dependencies. Production libraries are `libtilecomponents.so` and
`libmordred.so`; the old `libmittens.so` is not part of this model. Build products
live under `build/src`; the selected installation is under `install/src`.
`tools/hardware/env.sh` selects these artifacts explicitly without modifying the
shared SST registry. Build manifests record source and binary identities.

The correctness runner builds its test-only fixtures from current source, then
runs CPU, RVV, ordering, analog and network suites. The LLVM RVV suite compiles
fresh C kernels from its own fixtures. No correctness suite requires a study
directory or saved experiment binaries. No performance sweep is part of validation.

See the [validation record](migration-validation.md) for the component release's
builds, correctness coverage, profiling checks and the limits of those checks.

## Support moved with the model

The QEMU component owns its patches, device overlays and bridge headers.
`src/config/build` owns dependency pins; `src/platform` supplies retained startup,
linker and freestanding support. LLVM patches for the register analog instructions
and compiler support are recorded under `src/patches/llvm`; dependency builds
prepare a pinned copy instead of modifying a developer checkout.

The former custom router, memory-based analog bridge, global-memory controllers,
and old tile timing implementation are retired. Their platform/memory/network/
analog test groups are available in Git history. Older runtime and compiler
research fixtures outside the maintained suite remain historical references;
their results do not establish coverage of the current hardware. Legacy hardware
comparison commands fail with a retirement message instead of selecting an old
installation.

## Remaining application integration

Guest-controlled tile-to-tile sends are still pending. Current multi-tile tests
use a test controller to submit NIU requests. Guest CPUs access local SPM;
transfer payloads travel through SPM service, NIU, NIC and routers. A future guest
command interface must specify transfers and expose receive readiness while
preserving that payload path and finite storage credits.

Compiled Sculptor execution currently supports one tile. Connecting its multi-tile
runtime to the new NIU is separate follow-up work. This migration does not claim
that the old multi-tile software ABI is supported.
