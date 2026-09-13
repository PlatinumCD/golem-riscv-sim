# Data larger than SPM

One tile processes an input larger than its entire 16 KiB scratchpad. Code,
constants, a 4 KiB reusable data window, guard words and a 2 KiB reserved stack
all reside in SPM. Instructions use the 8 KiB I-cache; ordinary QEMU program
RAM is unmapped. Shared main memory has a fixed 4 MiB capacity.

```text
Shared RAM input → DMA → SPM window → scalar transform → DMA → Shared RAM output
                          repeat for each chunk
```

Cases: 64 KiB / 4 KiB chunks, 64 KiB + 12 B / 4 KiB chunks, and
128 KiB + 12 B / 3 KiB chunks. These cover exact division and partial tails.
Only one descriptor is active at a time. The program waits before modifying
or reusing its window. This is explicit chunking, not automatic paging.

Three separately traced phases prevent misleading traffic totals:

| Phase | Main memory → SPM | SPM → main memory |
|---|---:|---:|
| Input initialization | 0 | D |
| Processing | D | D |
| Output verification | D | 0 |

The guest generates input chunk-by-chunk and seeds shared RAM with modeled DMA;
the dataset is never embedded in the ELF. Verification rereads **every** output
word from shared RAM and checks it against an index-based reference. Guard words
and unused tail storage must remain intact. The host checker independently
checks actual DMA service bytes, address bounds, submitted/completed transfer
counts, boot traffic, and the linked code/data/stack allocation. Reported
allocation is reserved static storage, not a measured stack high-water mark.

Run `bash tests/run-all.sh --case memory/spm-chunking`.
Evidence is saved under `tests/results/` (isolated by the normal test runner).
This does not test oversized executable programs or compute/transfer overlap.
