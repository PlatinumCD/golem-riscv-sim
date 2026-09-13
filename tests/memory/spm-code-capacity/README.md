# Code and data compete for SPM

Fixed hardware: one tile, **32 KiB SPM**, **8 KiB I-cache**, eight 1R/1W
banks with 32-byte ports, 4 MiB shared main memory, and a 2 KiB reserved stack.
Code, constants, working data and guard words all occupy SPM. Guest program
RAM is unmapped. There is no paging or code overlay mechanism in this test.

For each executed code-body size (4, 12 and 20 KiB), the runner:

1. Links a probe and reads its real program/buffer/stack addresses.
2. Calculates the maximum 32-byte-aligned buffer that fits before the stack.
3. Runs buffers **32 bytes below** and **exactly at** that boundary.
4. Requires a buffer **32 bytes over** the boundary to fail linking with the
   specific `buffer overlaps reserved stack` error, without launching SST.

Thus nine cases mean six valid executions and three expected rejections.
The maximum is derived from the ELF, not a guessed code reservation. Code-body
size excludes startup and other runtime functions, which still consume SPM.

Every valid execution processes 64 KiB + 12 bytes from shared memory in chunks,
returns every result to shared memory and rereads every output for verification.
The code body performs a known number of additions; its returned value is
checked and used to transform the data. Guard words and unused chunk tails
must remain intact. DMA bytes and completion counts are checked independently
from traces. Boot, input initialization, processing and verification are separate.
Processing transfers D bytes in and D bytes out; setup and verification are
not free and are reported separately.

Run `bash tests/run-all.sh --case memory/spm-code-capacity`.
The normal runner saves per-case ELF/map, build/simulation logs, traces and
JSON results under `tests/results/test-runs/`. Probe ELFs are never executed.

The neighboring `platform/icache-working-set` test reuses this fixture but
holds the data window at 4 KiB and varies executed instruction footprint.
