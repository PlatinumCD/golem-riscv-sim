# Analog command queue validation and measured results

Reference revision: `c563e02` (selective LSQ dependencies already enabled). The promoted queue preserves the experiment’s functional implementation; the SST parameter description no longer labels it experimental. These measurements used an explicit depth-0 baseline. The current default is depth 4.

## Matched performance

Each case uses one square array, a matching matrix, LMUL8, 1000 resident-weight MVMs, 100-cycle compute, zero device-programming delay, 32 MiB SPM, four-byte banks, two 32-byte shared SPM channels, and shared VLEN/8 array-link bandwidth. The guest ELF and all architectural and transfer work are identical within each comparison.

| Array | VLEN | Banks | LSQ | Before cycles/MVM | ASQ4 cycles/MVM | Before programming cycles | ASQ4 programming cycles |
|---|---:|---:|---:|---:|---:|---:|---:|
| 32×32 | 1024 | 16 | 8 | 100.063 | 100.060 | 160 | 129 |
| 32×32 | 1024 | 16 | 16 | 100.063 | 100.060 | 144 | 129 |
| 256×256 | 512 | 8 | 8 | 129.092 | 108.113 | 14,134 | 10,038 |
| 256×256 | 512 | 8 | 16 | 106.138 | 100.129 | 12,086 | 10,038 |
| 256×256 | 1024 | 16 | 8 | 100.103 | 100.095 | 7,083 | 5,044 |
| 256×256 | 1024 | 16 | 16 | 100.103 | 100.095 | 6,059 | 5,035 |
| 1024×1024 | 1024 | 16 | 8 | 256.032 | 198.025 | 112,683 | 79,924 |
| 1024×1024 | 1024 | 16 | 16 | 240.032 | 189.043 | 96,299 | 79,915 |
| 2048×2048 | 512 | 16 | 8 | 734.032 | 466.034 | 720,937 | 434,229 |
| 2048×2048 | 512 | 16 | 16 | 662.032 | 403.034 | 639,019 | 376,884 |
| 2048×2048 | 1024 | 16 | 8 | 478.032 | 353.034 | 450,603 | 319,540 |
| 2048×2048 | 1024 | 16 | 16 | 430.032 | 333.034 | 385,067 | 319,531 |

ASQ depth 8 also measured 333.034 cycles/MVM and 319,531 programming cycles for 2048×2048 / VLEN1024 / 16 banks / LSQ16, identical to depth 4. These measurements establish overlap benefits for the tested kernels; they do not predict a general speedup. The model assumes dedicated analog register streaming ports subject to the unchanged shared array-link budget.

## Checks

- The original experiment passed 37 candidate runs across 12 configurations plus one fresh reference run. All 12 queue-disabled controls reproduced reference cycles exactly; all modes matched full SPM contents, instructions and transfer work.
- The promoted directed/admission suite passes nine plus three runs, including source release before programming delay, RAW/WAR/WAW, tails and SEW changes, captured stores, multiple arrays, byte limits, 170 backend Busy retries, and precise Accepted-to-Error trapping.
- Instruction budgets 1 and 256 produce identical phase/end cycles, full SPM and complete modeled traces after bijective command-token normalization.
- The portable backend suite passes eight enabled/disabled scenarios, checking reservations, finite resources, output bypass, capture timing and aggregate link bandwidth. The experiment additionally compared four disabled scenarios against the previous backend with byte-identical traces and statistics.
- All 24 configuration tests and five existing analog-register dependency regressions pass.

The portable tests rebuild guests from committed files and accept explicit QEMU/plugin paths. See [README.md](README.md) and the [backend suite](../analog-command-queue-backend/README.md) for reproduction commands. Full experiment artifacts remain under `experiments/analog-command-queue-20260923/`; promotion outputs are under `tests/results/source-new-analog-command-queue/`. They are local results, not test prerequisites.

The fresh promoted build reproduces the 2048×2048 / VLEN512 / 16 banks / LSQ16 / ASQ4 experiment exactly: **376,884 programming cycles and 403.034 cycles/MVM**, with identical ELF, full SPM and work. The 12 promoted directed/admission runs also match all 140 experimental raw modeled trace files byte for byte.
