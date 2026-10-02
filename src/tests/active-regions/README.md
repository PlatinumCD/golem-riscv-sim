# Active analog regions

Run the suite using separately built QEMU and component artifacts:

```sh
python3 src/components/riscv-qemu/build_qemu.py --output /tmp/active-regions/qemu --jobs 16
python3 src/tests/active-regions/run.py --output /tmp/active-regions/tests \
  --qemu /tmp/active-regions/qemu/qemu-system-riscv64
```

`--build-info` reuses a fingerprint-validated component build containing
`protocol.cc`. `--kind protocol` or `--kind rvv` limits the cases.

The direct-command fixture covers physical row strides, invalid bounds,
duplicate coverage, reconfiguration while busy/unread, repeated invocations,
long-to-short-to-long shape changes, input readiness and output retirement.
Its pipeline case fills both output slots, submits a third compute, and reads
one active row twice before releasing the first result. Deferred stores test
projected coverage, and a five-byte transfer buffer exercises byte tails.

Real `.insn`/RVV guests run at VLEN128/256/512 with analog queues on/off and
array pipelining on/off. A VLEN128/LMUL1 case spans multiple chunks per active
row. They check full-width slot rejection, changed shape operands through the
scalar LSQ, short output retirement and preservation of vector tail bits.
All cases verify actual link byte counts; initial-program proofs separately
verify active coverage and full physical matrix hashes with implicit zeros.
The original programming-delay/vector-analog suites provide compatibility
coverage for clients that never configure active extents.
