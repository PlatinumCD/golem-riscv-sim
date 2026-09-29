# Scalar arithmetic with outstanding vector work

This directed runtime test checks independent scalar arithmetic alongside real
QEMU/SST memory and array operations. It uses one CPU, one 64×64 array, VLEN 256,
LMUL 8, one four-byte SPM bank, LSQ depth 16 and ASQ depth 4. A 256-cycle array
programming delay exposes source-capture hazards.

Run with a current component plugin and QEMU build:

```sh
python3 -B src/tests/compressed-scalar/run.py \
  --output tests/results/scalar-lsq-check \
  --build-info build/src-scalar-lsq-final-20260929/build.json \
  --variants after \
  --after-qemu build/src-qemu-scalar-lsq-20260929/qemu-system-riscv64
```

Repeat with a fresh output directory and `--fault-bytes 4` to check a reserved
M word instruction instead of the reserved compressed instruction. Instruction
budgets 1 and 256 run by default; they must produce identical phase timing,
architectural state and CPU/SPM traces.

The first five measured phases check:

1. A pending vector load permits independent `C.ADDW`; the dependent vector
   arithmetic instruction still waits for every loaded register.
2. A pending vector store permits `C.ADDW`; a subsequent overlapping vector
   read observes the stored bytes in order.
3. An array program permits independent arithmetic while its source registers
   remain pinned until physical capture.
4. A pending array output permits scalar arithmetic; its vector consumer waits
   for the output to finish.
5. A reserved encoding drains both queues before trapping. All 16 directed
   memory entries and the array program must be outstanding at the boundary.
   The handler checks cause, fault PC, loaded registers and older store visibility.

Another 22 phases cover compressed OR, shifts, ANDI, SUB, XOR, AND and SUBW;
RV64 multiply, divide and remainder forms; upper and lower product halves;
word-result sign extension; divide by zero; and signed overflow. Every operation
has a fixed scalar result oracle and must execute while a vector load remains
outstanding. A following vector consumer must still wait for that load.

The runner validates queue capacity, token lifecycle, source capture, completion
order, all vector outputs and scalar results. A reserved RV64 word encoding with
`funct3=1` must not be mistaken for a legal M operation. Disabled extensions and
other unsupported encodings remain subject to QEMU's normal decoder and the
conservative queue drain.

Optional `--variants before after` also compares with the retained QEMU build
before the original scalar bypass; `--before-qemu` selects that binary. This is
a functional comparison using the same guest ELF and SST plugin, not an
application performance estimate. Executable and guest hashes are retained and
checked at completion.
