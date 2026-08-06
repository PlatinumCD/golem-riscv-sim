# Epoch D network timing-cell correction

Date: 2026-07-30

## Correction

Epoch C configured:

```text
network_cell_words   = 4096
network_buffer_cells = 4
```

At 32 bits and 1 GHz, this gave every request a minimum 16 KiB flow-control
unit and charged 4,096 cycles per cell, including five-word frame headers and
partial tensor bursts.

Epoch D separates physical transfer granularity from buffering:

```text
network_cell_words   = 1
network_buffer_cells = 16384
```

One timing cell is now exactly one architectural 32-bit word. Buffer capacity
remains unchanged at 64 KiB per configured Merlin buffer. The NIC ABI,
4,096-word maximum host burst, compiler routes, runtime frames, tensor
payloads, and QEMU device interface are unchanged.

## Validation

The following passed after the correction:

- Isolated network timing: 21/21 exact serialization, distance, and
  contention observations.
- Deployment runtime pair: one 300-word routed tensor through QEMU, SST, and
  receive DMA, with exact 46-cycle DMA service.
- Producer–MVM–recombine quick test: 4/4 placements.
- Four-token GPT-2 Greedy-Timing-L3 analog deployment: all 61 active tiles.
- Four-token GPT-2 Greedy-Timing-L3 digital deployment: all 64 active tiles.

The isolated network remains governed by:

```text
head = 35 + 12 * (Manhattan hops - 1)
completion = head + payload words - 1
```

## Focused GPT-2 comparison

The comparison uses backend-specific Greedy-Timing-L3 placements, dual-issue
1 GHz CPUs, native memory, an 8x8 mesh, four input tokens, and the fixed
Epoch D one-million-instruction temporal-lookahead quantum.

| Backend | Epoch C timing cell | Epoch D timing cell | Change |
|---|---:|---:|---:|
| Analog | 19.243900 ms | 11.971912 ms | -37.79% |
| Digital | 120.132000 ms | 117.758255 ms | -1.98% |

The corresponding analog-over-digital speedup changes from 6.24x to 9.84x
for this pair. This is not a new analog hardware optimization. It removes
network time that Epoch C charged for nonexistent padding.

Analog benefits more from the correction because communication occupies a
larger fraction of its exposed critical path. The digital deployment remains
dominated by 329,454,882 retired instructions.

## Receive-side temporal-lookahead audit

A 24-route, two-tile blocking fanout used identical traffic at five QEMU/SST
instruction quanta:

| Instruction quantum | Completion | Retired instructions | CPU cycles |
|---:|---:|---:|---:|
| 1,000,000 | 36.711 us | 98,393 | 49,247 |
| 100,000 | 36.711 us | 98,393 | 49,247 |
| 10,000 | 36.191 us | 97,355 | 48,727 |
| 1,000 | 36.188 us | 97,351 | 48,724 |
| 100 | 36.188 us | 97,351 | 48,724 |

Every trial passed, transferred 12,408 words over 12,408 word-hops, and
reported no router stalls or transmit blocking. The 1,000- and
100-instruction results are identical, while the frozen Epoch D quantum is
523 ns (1.45%) slower.

The difference is not a network-cell error. With a coarse grant, QEMU can
execute a software receive scan before SST publishes a packet whose simulated
arrival falls inside that grant. The scan accounts for 1,042 additional
retired instructions on the destination tile. The current co-simulation has
no rollback, so `sync_instruction_quantum` is a temporal-lookahead accuracy
parameter for receive-dependent control flow.

Consequently, the focused GPT-2 values above are valid results for the exact
frozen Epoch D configuration, but they are not yet quantum-converged
paper-quality makespans. The physical-word network correction itself remains
validated independently by the exact 21-case SST-only network test.

## Evidence boundary

Epoch C remains frozen for reproduction of prior experiments. Its component
tests remain useful, and its transmit-doorbell synchronization conclusion
remains valid, but absolute model-level network and makespan results use the
superseded coarse timing cell. Receive-side temporal lookahead remains an
explicit accuracy boundary in Epoch D.

New architectural sweeps must identify Epoch D and use:

```text
config/epoch-d.env
```

Focused artifacts are under:

```text
build/research/epoch-d-gpt2-greedy-timing-l3/
├── analog/
└── digital/
```
