# Analog register dependency regressions

Run the standalone suite against the current built model:

```sh
python src/tests/analog-register-dependencies/run.py \
  --build-info build/src/components/build.json \
  --qemu build/src/qemu/qemu-system-riscv64
```

The default five runs cover VLEN 128, 256 and 1024 at LSQ depth 16, a VLEN-256
depth-1 control, and a VLEN-256/depth-16 replay with host instruction budget 1
instead of 256. `--vlen 256` narrows coverage to three runs while retaining both
controls. `--compiler` selects the Golem LLVM compiler. Runs execute sequentially.

Each guest programs a real identity matrix and checks twelve directed phases:

- `mvm.vset` and `mvm.vl` preserve load-to-source RAW dependencies.
- `mvm.vs` preserves load-to-destination WAW dependencies.
- Unrelated loads overlap all three transfers and scalar `mvm` computation.
- Pending stores retain their captured payload when `mvm.vs` overwrites registers.
- m8 upper-register aliases, fractional mf2 destinations, and short-VL tails
  preserve both active results and inactive register contents.
- Dynamic bad-array and bad-offset traps occur with older load/store work still
  pending, but the handler sees both committed before its first instruction fetch.
- Invalid register-group encodings retain conservative predecode ordering.

The model uses one array, a two-MiB SPM, one four-byte bank, two 32-byte channels,
zero programming delay and a 100-cycle MVM. Array pipelining is disabled so
repeated output reads inspect the same result. Matrix dimensions 32, 64 and 256
fill one m8 FP32 register group at the respective VLEN. Full real weight
programming precedes the checks; there is no injected array state.

Validation includes exact results and tails, complete final SPM equivalence
between blocking and queued execution, precise trap records, LSQ lifecycle/FIFO
retirement, marker completion and actual overlap intervals. Host-budget replay
must preserve every modeled event and cycle. Analog command token IDs are
compared after one bijective, order-preserving renumbering because extra host
grants consume bridge sequence IDs; every other analog field is retained and
non-analog CSV traces must be byte-identical. Only host grant/stop counters may
differ.

Optional `--baseline-build-info PATH --baseline-qemu PATH` adds a historical model
that globally drained the LSQ before analog operations. Those runs must preserve
the expected drain behavior, while matching current outputs, complete SPM and
work counts. The depth-1 pair must have identical raw traces and timing. The
historical source tree need not remain unchanged: baseline binaries and the
original manifest are fingerprinted; current-model source fingerprints must
still match their build manifest.

Results default to `tests/results/source-new-analog-register-dependencies/`.
Use `--output PATH` to choose a new directory. Recheck retained results without
launching simulations with `--check-only --output PATH`. Each run retains its
configuration, ELF metadata, traces and checks; the top-level `validation.json`
records suite comparisons. These directed-test runtimes include setup, fences
and trap handlers and are not application throughput measurements.
