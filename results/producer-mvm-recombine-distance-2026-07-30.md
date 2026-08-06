# Producer–MVM–recombine distance study

Date: 2026-07-30

## Question

How much end-to-end latency is added when a 512-word activation producer, a
two-array analog MVM, and a two-partial recombination task are separated by
increasing Manhattan distance?

## Controlled configuration

- Fixed 9x9 mesh.
- One 32-bit timing cell per 1 GHz mesh-link cycle.
- 16,384-word (64 KiB) router buffering.
- 1 GHz dual-issue RISC-V tiles.
- Native memory backend.
- Two 256x512 analog arrays.
- One shared 256-bit, 1 GHz analog link.
- Eight-cycle analog execution.
- Warm, already-programmed matrices.
- One execution and no unrelated traffic.
- Horizontal producer-to-MVM routes and vertical MVM-to-recombine routes.

The producer emits one 512-word activation. The MVM tile produces two
256-word partial results. Recombination consumes both partials.

## Result

All 35 placements passed numerical validation, task-order validation, analog
command validation, route-size validation, receive-DMA validation, and exact
word-hop accounting.

| Input distance | Output distance | Total | Input boundary | Output boundary | Data word-hops |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 516 ns | 4 ns | 3 ns | 0 |
| 1 | 0 | 1,156 ns | 644 ns | 3 ns | 512 |
| 8 | 0 | 1,241 ns | 729 ns | 3 ns | 4,096 |
| 0 | 1 | 1,134 ns | 4 ns | 620 ns | 512 |
| 0 | 8 | 1,218 ns | 4 ns | 704 ns | 4,096 |
| 1 | 1 | 1,774 ns | 644 ns | 620 ns | 1,024 |
| 8 | 8 | 1,943 ns | 729 ns | 704 ns | 8,192 |

The local work is stable:

- Producer: 226 ns.
- Two-array MVM task: 205 ns, including 198 ns of analog service.
- Recombination: 78-79 ns.

For nonzero distances, least-squares fits give:

```text
activation boundary = approximately 631 ns + 12.22 ns * hops
partial boundary    = exactly        608 ns + 12.00 ns * hops
```

The first tile boundary is therefore more important than extra distance in
this uncontended experiment. A one-hop boundary adds approximately 0.62-0.64
microseconds, while increasing the path from one to eight hops adds only
84-85 ns. The activation and partial boundaries are nearly additive in the
combined surface.

This does not show that Manhattan distance is generally unimportant.
Contention, shared turns, concurrent executions, and competing flows are
deliberately absent. It establishes the uncontended baseline needed to
measure those effects separately.

## Configuration issue exposed during development

An exploratory run inherited `network_cell_words=4096` from the current GPT-2
deployment. That makes Merlin's minimum timing cell 4,096 words even for a
5-, 256-, or 512-word request. The eight-hop activation boundary measured
82.277 microseconds instead of 729 ns, and the two-partial boundary measured
88.181 microseconds instead of 704 ns.

The accepted study uses `network_cell_words=1` and
`network_buffer_cells=16384`, separating the physical 32-bit transfer unit
from the original 64 KiB router capacity. Current model-level deployment
scripts use the same corrected configuration. The frozen Epoch C manifest and
its earlier GPT-2 results retain the 4,096-word timing cell for historical
reproducibility and must not be compared directly with the corrected
physical-word baseline.

## Artifacts

Source and runner:

```text
tests/producer-mvm-recombine-distance/
```

Machine-readable full result:

```text
build/tests/producer-mvm-recombine-distance/runs/full/results.csv
```

Each placement also retains raw per-tile task, network, receive-DMA, analog,
wait, and summary profiles plus router statistics and the foreground
simulation log.
