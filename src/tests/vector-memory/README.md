# Vector memory regressions

Run `python3 -B src/tests/vector-memory/run.py` after building the isolated
QEMU. The runner builds an SST plugin with the independent peer test driver.
`--build-info PATH/build.json` reuses a matching plugin that already includes
`tests/riscv-qemu/peer.cc`, after checking its recorded source hashes.
Results default to `tests/results/source-new-vector-memory/<time>/`.

Fourteen configurations cover VLEN 256/512, EEW 8/16/32/64, LMUL m1/m2/m8,
and counterfactual one-bank/eight-byte-channel configurations. Each guest checks
aligned full vectors, a scalar reference, tails, alternating masks, element
misalignment, nonzero vstart, genuinely strided fallback, and two-register whole
loads/stores. A masked invalid suffix must not fault. Unmasked accesses across
the end of SPM must commit the valid prefix and trap with the expected cause,
fault address, and vstart for both loads and stores. A locked PMP region within
a page checks that permission preflight cannot authorize a protected suffix.
A vector store patches previously executed code, followed by `fence.i` and a
call that must return the new value. Whole-register transfers run with VL=1 to
check that they still transfer complete registers.

Task regions and `riscv-memory.csv` identify actual pre-access memory beats.
The bank trace must show aligned 32-byte requests served as eight four-byte
bank operations in one cycle. A 512-bit beat becomes two concurrent 32-byte
requests served over two cycles. LMUL groups issue multiple VLEN-sized beats.
The one-bank and narrow-channel controls must lengthen backend service without
changing functional output. Checks distinguish scalar four-byte accesses from
coalesced vector accesses and preserve masked gaps.

An independent StandardMem peer first writes inputs, reads 40 output bytes,
keeps same-line reads queued around the CPU's vector output and ready stores, and patches previously
executed code. The CPU observes the patch after `fence.i`. Peer traffic finishes
before the uncontended vector bank probes. Raw traces and the shared SPM image
are checked independently of the guest's own numerical checks. Polls ordered
before the vector write must see zero; polls after its bank completion must
see the committed vector result, including queued reads held until QEMU commits.
