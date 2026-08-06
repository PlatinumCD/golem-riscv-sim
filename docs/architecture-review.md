# Golem RISC-V Simulator Architecture Review

This document describes the simulated architecture, its current experimental
configuration, the rates used by each timing model, the parameters that can be
changed, the validation evidence, and the limits on the claims that can
currently be made.

The concise description of the system is:

> The simulator models a bare-metal tiled accelerator in which every tile
> contains one RV64GCV hart, private memory, a mesh NIC, a receive DMA engine,
> and local analog MVM arrays. QEMU executes each tile's real RISC-V ELF while
> SST models time, routing, contention, DMA service, analog transfers, and
> accelerator execution.

This is not yet a cycle-accurate CPU or a calibrated silicon model. The
network, analog-link scheduler, receive DMA, and synchronization follow
explicit timing models. The CPU is a retired-instruction throughput model,
and the current GPT-2 experiments use untimed native RAM.

## 1. Overall simulated system

```text
PyTorch model
      |
      v
Torch-MLIR
      |
      v
Sculptor
  - extracts tasks
  - partitions MVMs
  - builds the task graph
  - places tasks and arrays
  - optionally builds reduction trees
  - partitions the program by tile
      |
      v
One bare-metal RISC-V ELF per active tile
      |
      v
+------------------------- SST simulation --------------------------+
|                                                                   |
|  Tile 0 <---> Router <---> Router <---> ... <---> Router <--> N  |
|    |                                                              |
|    +-- one QEMU RV64 hart                                         |
|    +-- private guest RAM                                          |
|    +-- mesh NIC                                                   |
|    +-- receive DMA                                                |
|    +-- local analog arrays                                        |
|                                                                   |
|  SST owns simulated time, topology, routing, contention, DMA,     |
|  analog-link arbitration, and analog execution latency.           |
|                                                                   |
|  QEMU owns instruction semantics, registers, guest-memory bytes,  |
|  RVV execution, and custom Golem instruction decoding.            |
+-------------------------------------------------------------------+
```

For an `R x C` mesh:

```text
tile_id = y * C + x
x       = tile_id % C
y       = tile_id / C
```

Every tile has its own QEMU process and private address space. Guest RAM is
never shared between tiles. Inter-tile data must pass through the simulated
mesh.

## 2. Current 12x12 GPT experiment

The current 12x12 research configuration now inherits Epoch E's 100 ns MVM
cost and otherwise uses the following parameters. Epoch E's reference
manifest retains an 8x8 default topology, which deployment scripts may
override. The latest stored 12x12 trace was collected under the historical
eight-cycle Epoch D analog latency and is labeled separately in Section 11.

| Property | Current value |
| --- | ---: |
| Mesh | 12 x 12 |
| Physical tiles and routers | 144 |
| Active compute tiles | 66 |
| Hart count | 1 per tile |
| CPU clock | 1 GHz |
| Scalar issue width | 2 |
| RVV | 1.0 |
| VLEN / ELEN | 256 / 64 bits |
| Private QEMU RAM | 64 MiB per tile |
| Memory timing backend | Native, untimed |
| Network link | 32 bits at 1 GHz |
| Link rate | 4 GB/s per directed link |
| Link latency | 10 ns |
| Routing | Deterministic X-then-Y |
| Router buffering | 16,384 words = 64 KiB |
| Receive DMA | 256 bits at 1 GHz |
| DMA setup | 8 cycles per descriptor |
| Analog arrays | 4 per tile |
| Array dimensions | 1024 x 512 |
| Analog link | One shared 256-bit half-duplex link per tile |
| Analog link rate | 32 GB/s aggregate per tile |
| Analog compute latency | 100 ns per MVM |
| Analog backend | Native float32 |
| Instruction quantum | 1,000,000 |
| Profile mode | Trace |
| Scheduler | Greedy-Timing, beam 8 |
| Heuristics | Transfer cost, boundary regret, compact region, link pressure, diagonal scope |
| Reduction width | 2 |

The current workload is specifically a GPT-2-small-shaped compiler fixture:

- 12 transformer layers;
- hidden size 768;
- 12 attention heads;
- feed-forward size 3072;
- batch size 1;
- static sequence length 4;
- deterministic synthetic weights; and
- direct hidden-state input.

It is not yet the complete pretrained GPT-2 language-model path. It omits the
tokenizer, token embedding, positional embedding, and language-model head from
the compiled deployment.

## 3. Contents of one tile

```text
+-------------------------- ONE TILE ----------------------------+
|                                                               |
|  Bare-metal RV64GCV hart                                      |
|    - one hardware thread                                      |
|    - RVV 1.0, VLEN=256, ELEN=64                               |
|    - custom XGolemAnalog ISA                                  |
|    - task runtime and task registry                           |
|                                                               |
|  Private QEMU RAM                                             |
|    - ELF text/data                                            |
|    - tensor workspace                                         |
|    - task state                                               |
|    - no shared address space                                  |
|                                                               |
|  Mesh NIC                                                     |
|    - 32-bit architectural words                               |
|    - 5-word runtime frame header                              |
|    - up to 4096 words per host-side burst                     |
|                                                               |
|  Receive DMA                                                  |
|    - one channel                                              |
|    - 256-bit transfers                                        |
|    - four-entry incoming queue                                |
|                                                               |
|  Analog subsystem                                             |
|    - array 0                                                  |
|    - array 1        independent compute engines               |
|    - array 2                                                  |
|    - array 3                                                  |
|          \                                                    |
|           +-- one shared 256-bit half-duplex analog link      |
|                                                               |
|  Optional private L1 timing path                              |
|    - 32 KiB, 4-way, 64-byte line                              |
|    - currently disabled in the GPT baseline                   |
+---------------------------------------------------------------+
```

The runtime starts by executing its boot registry. Boot tasks program each
tile's local analog matrices. It then waits for input routes, marks local
resources ready, executes ready tasks, and forwards outputs according to the
compiled route table.

The current deployment executes one inference flow with `execution_id = 0`.
The frame ABI carries a 64-bit execution ID, but concurrent independent
execution IDs are not yet exercised by the runtime.

## 4. CPU timing

QEMU reports two counters at every synchronized boundary:

- total retired instructions; and
- retired vector instructions.

SST charges:

```text
CPU cycles =
    max(
        ceil(total retired instructions / cpu_issue_width),
        retired vector instructions
    )
```

At the current 1 GHz and issue width 2:

```text
scalar throughput ceiling = 2 billion instructions/second/tile
vector issue ceiling      = 1 billion vector instructions/second/tile
```

A vector instruction counts in the total instruction count and is also
subject to the one-vector-instruction-per-cycle constraint.

For example:

```text
1000 total instructions
 200 vector instructions
 issue width = 2

scalar limit = ceil(1000 / 2) = 500 cycles
vector limit = 200 cycles

charged CPU time = 500 cycles = 500 ns at 1 GHz
```

This model captures real compiler-generated instruction counts, including
runtime code, scalar work, and RVV instructions. It does not yet model:

- instruction dependencies;
- operation-specific latency;
- branch prediction;
- pipeline hazards;
- reorder buffers;
- functional-unit occupancy;
- vector length or LMUL-dependent execution time;
- instruction-cache stalls; or
- data-cache stalls in native-memory mode.

Therefore, dual issue means a modeled retirement-throughput ceiling. It does
not mean QEMU has become a modeled dual-issue microarchitecture.

Changing only the CPU clock from 1 to 3 GHz makes these CPU intervals three
times shorter. The network, DMA, and analog domains remain at their
independently configured clocks unless those clocks are also changed.

## 5. RISC-V vector architecture

The conforming architecture is:

```text
ISA              RV64GCV + XGolemAnalog
VLEN             256 bits
ELEN             64 bits
Vector registers 32
Vector issue     at most one instruction/cycle
```

For `LMUL=1`, one vector register holds:

| Element type | Elements per 256-bit register |
| --- | ---: |
| i8 | 32 |
| i16 | 16 |
| i32 / f32 | 8 |
| i64 / f64 | 4 |

The compiler cost model currently assumes a 256-bit digital vector datapath.
The SST CPU timing, however, still uses retired vector instruction count
rather than a separate per-operation vector latency model.

That distinction is important:
`digital-vector-bits-per-cycle=256` helps Sculptor estimate placement, while
the actual simulation charges the QEMU-generated instruction sequence.

## 6. Mesh network model

The mesh uses one Merlin router per tile, with up to five ports:

```text
north
  |
west -- router -- east
  |
south

plus one local tile port
```

Routing is deterministic dimension-order routing:

1. Route in X until the destination column is reached.
2. Route in Y until the destination row is reached.
3. Eject through the local port.

There is no adaptive routing or compiler-selected path. The compiler selects
the destination tile; the network selects the deterministic Manhattan route.

### 6.1 Current link rate

```text
width = 32 bits
clock = 1 GHz

rate = 32 Gbit/s = 4 GB/s per directed link
```

Every tensor is transported as ordered 32-bit words. Increasing the host
burst size does not increase physical link bandwidth.

The runtime sends a five-word header:

```text
word 0: frame magic
word 1: route ID
word 2: execution ID, low 32 bits
word 3: execution ID, high 32 bits
word 4: payload word count
```

The payload follows as one or more bursts of at most 4096 words. The
4096-word limit is host batching, not a 4096-word physical flit.

### 6.2 Validated latency law

For the current 32-bit, 1 GHz, 10 ns-link configuration:

```text
head arrival =
    35 + 12 * (Manhattan hops - 1) cycles

packet completion =
    head arrival + packet_words - 1 cycles
```

Examples:

| Packet | Distance | Completion |
| --- | ---: | ---: |
| 1 word | 1 hop | 35 ns |
| 512 words | 1 hop | 546 ns |
| 512 words | 4 hops | 582 ns |
| 3072 words | 1 hop | 3.106 us |

Contention adds serialization and router stalls. Equal-sized one-hop incast
obeys:

```text
completion[rank] =
    35 + (rank + 1) * packet_words - 1 cycles
```

The network models:

- physical serialization;
- Manhattan distance;
- router and link latency;
- input/output buffering;
- arbitration;
- link contention;
- credits and backpressure; and
- endpoint tail completion.

## 7. Receive DMA

When a header arrives, the runtime identifies the destination tensor and
registers a DMA descriptor. The payload then moves:

```text
source guest RAM
      |
      v
source QEMU snapshot
      |
      v
32-bit SST mesh
      |
      v
destination receive DMA
      |
      v
destination guest RAM
```

Current DMA parameters:

```text
clock       = 1 GHz
width       = 256 bits = 8 float32 words/cycle
setup       = 8 cycles per descriptor
queue depth = 4
channels    = one per tile
```

Service time for one burst is:

```text
DMA cycles = 8 + ceil(payload_words / 8)
```

Examples:

| Payload | DMA time |
| ---: | ---: |
| 512 words | 72 ns |
| 1024 words | 136 ns |
| 3072 words | 392 ns |

Different tiles' DMA engines run concurrently. Bursts targeting the same tile
serialize through its one DMA channel.

The DMA does not eliminate NoC traffic. It eliminates destination-side
per-word guest MMIO handling.

## 8. Analog accelerator model

Each tile currently has four arrays. Every array is a fixed 1024x512 float32
MVM device.

Supported custom instructions are:

| Instruction | Function |
| --- | --- |
| `mvm.set` | Program an array matrix |
| `mvm.l` | Load an input vector |
| `mvm` | Start computation |
| `mvm.s` | Wait for and store output |
| `mvm.mv` | Move output between local arrays |

Every array has:

- its own matrix;
- its own input and output state;
- its own ordered four-entry command queue; and
- an independent compute engine.

All four arrays share one 256-bit half-duplex link:

```text
array 0 ---+
array 1 ---+
array 2 ---+--> round-robin --> one 256-bit link --> tile RAM boundary
array 3 ---+
```

At 1 GHz:

```text
256 bits/cycle = 32 bytes/cycle = 32 GB/s aggregate per tile
```

That rate does not multiply by four when four arrays are present.

### 8.1 Current operation costs

One link beat contains eight float32 words.

For a 1024x512 array:

| Operation | Words | Link cycles | Time |
| --- | ---: | ---: | ---: |
| Program matrix | 524,288 | 65,536 | 65.536 us |
| Load vector | 512 | 64 | 64 ns |
| Compute | 0 | 100 configured cycles | 100 ns |
| Store vector | 1024 | 128 | 128 ns |

An isolated loaded MVM therefore takes approximately:

```text
64 ns input + 100 ns compute + 128 ns output = 292 ns
```

That excludes matrix programming and CPU instruction time.

Programming all four full arrays on one tile requires:

```text
4 x 2 MiB = 8 MiB
4 x 65.536 us = 262.144 us
```

because the four programming transfers share the link.

The fixed 100-cycle compute latency mathematically implies:

```text
1024 x 512 MACs / 100 ns
= 5.24288 TMAC/s per array
```

Four compute engines imply 20.97152 TMAC/s if all four are independently
computing. That is an implication of the configured abstract latency, not a
physically validated throughput claim. Actual system throughput is reduced by
the shared link, CPU work, task dependencies, and NoC traffic.

### 8.2 Functional backends

Two numerical backends exist:

- `native`: C++ float32 MVM; and
- `crosssim`: CrossSim numerical behavior and nonidealities.

SST supplies the same timing model to both. CrossSim host execution time does
not become simulated time. CrossSim currently changes numerical behavior, not
the 100-cycle accelerator timing.

## 9. Memory model

There are two modes.

### 9.1 Native memory: current GPT baseline

QEMU owns all private RAM and accesses it directly.

```text
memory latency added by SST = zero
```

Loads and stores still generate retired CPU instructions, but there are no
cache misses, DRAM delays, bank conflicts, or bandwidth limits.

This means the current analog-versus-digital comparison can understate memory
pressure, especially for digital tensor operations.

### 9.2 Optional memHierarchy backend

Each tile receives a private:

- 32 KiB L1;
- 4-way associativity;
- 64-byte cache lines;
- LRU replacement;
- 1 GHz cache clock;
- 2-cycle configured lookup;
- 1 ns links; and
- 50 ns simple lower memory.

The complete validated path observes approximately:

```text
L1 hit  = 5 cycles
L1 miss = 61 cycles
```

QEMU remains the owner of functional bytes. SST tracks only timing and cache
state.

The current implementation allows one outstanding CPU data-memory request per
tile. It does not yet model:

- nonblocking caches or MSHRs;
- instruction caches;
- TLBs;
- prefetching;
- scratchpads;
- DRAM banks/controllers; or
- cache/DMA coherence.

Initialization may be aggregated as:

```text
initialization time =
    2 cycles + ceil(initialization_bytes / 32 bytes-per-cycle)
```

That initialization model avoids millions of QEMU/SST handshakes while
retaining total traffic volume. It does not populate realistic L1 state.

## 10. Compiler and runtime behavior

The compiler produces a static task graph with fixed tensor shapes.

Each active tile receives an ELF containing:

- its local task functions;
- a boot-task registry;
- a normal task registry;
- local resource descriptors;
- incoming and outgoing routes;
- physical and local analog-array IDs;
- tensor workspace; and
- the bare-metal runtime.

At boot:

```text
boot tasks
    |
    +--> program local matrices using mvm.set
    |
    v
runtime ready
```

During execution:

```text
receive route header
    |
register receive DMA
    |
payload arrives
    |
mark local input ready
    |
check task readiness
    |
execute task
    |
route outputs to destination tiles
```

Task placement is static. There is no operating-system scheduler migrating
tasks at runtime.

The runtime is bare metal:

- no Linux;
- no processes inside a tile;
- no pthreads;
- no OpenMP;
- no virtual memory;
- no NIC completion interrupts; and
- event-driven blocking doorbells rather than polling.

## 11. Latest measured result

The latest stored balanced 12x12, four-token, analog trace predates Epoch E
and used the historical eight-cycle analog compute latency. Its results are:

| Metric | Value |
| --- | ---: |
| Simulated makespan | 12.013709 ms |
| Active tiles | 66 |
| Tasks | 1,837 |
| Routes | 1,382 |
| Retired instructions, aggregate | 53,683,072 |
| Vector instructions, aggregate | 8,717,929 |
| CPU cycles, aggregate | 26,844,563 |
| Analog operations | 3,120 |
| Matrix setup operations | 240 |
| Vector loads | 960 |
| MVM computes | 960 |
| Output stores | 960 |
| Analog link beats, aggregate | 15,912,960 |
| Injected network words | 937,726 |
| Injected bytes | 3.751 MB |
| Directional word-hops | 1,752,108 |
| Physical router-link traffic | 7.008 MB |
| Router stalls | 80,643 |
| Receive-DMA transfers | 1,382 |
| Receive-DMA words | 930,816 |
| Output elements | 3,072 finite float32 values |

Approximately 98.84% of the analog-link beats are matrix programming:

```text
240 matrices x 524,288 words / 8 words-per-beat
= 15,728,640 programming beats
```

The remaining beats carry 960 input vectors and 960 output vectors.

This run includes cold-start weight programming. It is not steady-state
inference with weights already resident.

Aggregate component cycles must not be added together. Tiles, routers, DMA
engines, and analog arrays overlap. The 26.8 million aggregate CPU cycles and
15.9 million aggregate analog cycles occur across many tiles; the global
makespan is the critical path, not their sum.

### 11.1 Reduction-balancing comparison

Against the matching run without reduction balancing:

| Metric | Balanced | Unbalanced | Change |
| --- | ---: | ---: | ---: |
| Makespan | 12.013709 ms | 12.078960 ms | -0.540% |
| Active tiles | 66 | 68 | -2 |
| Injected words | 937,726 | 952,986 | -1.60% |
| Word-hops | 1,752,108 | 1,788,353 | -2.03% |
| Router stalls | 80,643 | 209,721 | -61.55% |

The main benefit is reduced contention, not a large reduction in CPU work.

## 12. Parameters that can be modified

### 12.1 Hardware and timing parameters

| Parameter | Current | Constraint or meaning |
| --- | ---: | --- |
| `mesh_width`, `mesh_height` | 12x12 | Any positive rectangular mesh |
| `cpu_clock` | 1 GHz | CPU timing domain |
| `cpu_issue_width` | 2 | Only 1, 2, or 4 |
| `riscv_vector_enabled` | true | Enables RVV in QEMU |
| `riscv_vector_length_bits` | 256 | Power of two, 128-1024 |
| `riscv_vector_element_bits` | 64 | Power of two, 8-64 |
| `memory` | 64 MiB | Private QEMU RAM per tile |
| `mesh_link_clock` | 1 GHz | NoC serialization clock |
| `mesh_link_width_bits` | 32 | Positive multiple of 32 |
| Mesh link latency | 10 ns | Currently fixed in the mesh builder |
| `network_cell_words` | 1 | Physical arbitration cell; keep 1 for v0.1 |
| `network_buffer_cells` | 16,384 | Current 64 KiB buffering |
| `rx_dma_clock` | 1 GHz | DMA timing domain |
| `rx_dma_width_bits` | 256 | Positive multiple of 32 |
| `rx_dma_setup_cycles` | 8 | Per receive descriptor |
| `rx_dma_queue_depth` | 4 | 1-4 |
| `analog_array_count` | 4 | Arrays instantiated on every tile |
| `analog_array_rows` | 1024 | Fixed for all arrays |
| `analog_array_columns` | 512 | Fixed for all arrays |
| `analog_link_clock` | 1 GHz | Shared-link clock |
| Analog link width | 256 bits | Fixed by current bridge ABI |
| `analog_compute_latency_cycles` | 100 | Fixed delay per MVM |
| `analog_backend` | native | `native` or `crosssim` |
| `crosssim_config` | default | CrossSim nonideality configuration |
| `memory_backend` | native | `native` or `memhierarchy` |

### 12.2 Compiler and placement parameters

The compiler can vary:

- analog versus digital MVM lowering;
- model sequence length;
- mesh dimensions;
- number of schedulable cores;
- arrays per core;
- array rows and columns;
- reduction width;
- random seed;
- snake, random, greedy, or greedy-timing scheduling;
- lookahead depth;
- beam width;
- transfer-cost scoring;
- compact-region scoring;
- boundary-regret scoring;
- link-pressure scoring;
- placement scope;
- whether reduction balancing is enabled; and
- analog and digital scheduler cost models.

The compiler currently estimates:

```text
analog MVM latency       = 100 ns
analog I/O               = 256 bits/cycle, shared
digital clock            = 1 GHz
digital issue width      = 2
digital vector width     = 256 bits/cycle
network width            = 32 bits/cycle
network hop latency      = 10 cycles
network behavior         = pipelined
```

These compiler estimates must remain aligned with the SST configuration.
Otherwise, the compiler optimizes for one machine while SST simulates another.

### 12.3 Experiment and instrumentation parameters

| Control | Effect |
| --- | --- |
| `profile_mode=off` | No profiling |
| `profile_mode=summary` | Unperturbed finish counters |
| `profile_mode=trace` | Task/device/network timelines; task markers perturb execution |
| Visualization export | Produces a compact activity trace |
| Blocking/async transmit | Whether ready tasks may overlap blocked sends |
| SST thread count | Changes host wall time, not intended simulated time |
| Build-job count | Compile wall time only |

`sync_instruction_quantum` needs special treatment. It controls how far QEMU
can run before returning to SST. It should ideally be only a host-performance
parameter, but receive-side observation is not yet completely
quantum-independent.

The controlled fanout test converged to:

```text
36.188 us at quantum 1,000
36.188 us at quantum 100
36.711 us at quantum 100,000 or 1,000,000
```

That is a 1.45% difference. Therefore, the current one-million-instruction
quantum must be recorded as part of the experimental configuration until the
receive-side synchronization limitation is removed or every result includes a
convergence study.

## 13. Host resource scaling

The 12x12 simulation creates 144 QEMU/SST tiles, even though only 66 currently
execute non-idle task ELFs.

Configured QEMU RAM alone is:

```text
144 x 64 MiB = 9 GiB
```

The analog shared-memory bridge allocates approximately:

```text
4 arrays
x 4 command slots
x 1024 x 512 float32 words
= approximately 32 MiB per tile

144 tiles = approximately 4.5 GiB
```

The obvious lower bound is therefore already about 13.5 GiB before QEMU
overhead, SST routers, native matrix storage, ELF mappings, trace buffers, and
CrossSim state.

That is host memory consumption, not simulated chip memory capacity.

## 14. Validation status

The validation suite currently shows that the implementation follows its
declared model:

- CPU: 18/18 exact instruction/cycle comparisons across issue widths and
  quanta.
- Network: 21/21 exact head and tail timestamps across packet size, distance,
  and incast.
- Analog: exact single- and dual-array queue, link, compute, and overlap
  schedules.
- Memory: exact private-L1 hit, miss, conflict, capacity, write, and LRU
  behavior.
- Transmit synchronization: event-driven doorbells and bounded backpressure.
- End-to-end: real compiler-generated per-tile ELFs execute and exchange
  tensor data.

This validates simulator consistency. The focused analog timing fixture uses
an eight-cycle microconfiguration to validate the mechanism; it does not
validate that Epoch E's selected 100-cycle MVM latency, the abstract CPU
pipeline, or the chosen network parameters match fabricated silicon.

## 15. Responsible claim boundary

The current system can responsibly claim:

- real bare-metal RISC-V programs execute on every active tile;
- RVV and custom analog instructions are functionally decoded by QEMU;
- data moves through a contention-aware 32-bit SST mesh;
- analog arrays have independent compute engines and a shared finite-width
  link;
- receive DMA has explicit setup, bandwidth, serialization, and queue depth;
- task placement and reduction structure materially affect traffic and
  contention; and
- the system records instructions, tasks, packets, word-hops, DMA work,
  analog work, waits, and critical paths.

The current system must not yet claim:

- cycle-accurate CPU performance;
- a validated dual-issue or quad-issue pipeline;
- realistic cache/DRAM behavior in native-memory experiments;
- physically validated analog latency or throughput;
- power, energy, area, or thermal accuracy;
- that CrossSim host execution time represents hardware latency;
- that the four-token fixture is full pretrained GPT-2 inference; or
- that the traced 12.013709 ms is the final publication number.

The safest overall characterization is:

> The simulator is currently strongest as a deterministic architectural
> exploration environment for task placement, mesh communication,
> analog-array sharing, receive DMA, and compiler/runtime interaction. Its NoC
> and device timing are explicit and validated against closed-form tests.
> Absolute processor and memory performance still depends on deliberately
> simplified CPU and native-memory models, so those are the next fidelity
> boundaries before making silicon-level latency claims.

## 16. Authoritative supporting documents

- [`config/epoch-e.env`](config/epoch-e.env) records the current 100 ns MVM
  deployment parameters.
- [`config/epoch-d.env`](config/epoch-d.env) records the frozen historical
  eight-cycle physical-word-network parameters.
- [`docs/timing-model.md`](docs/timing-model.md) defines the detailed timing
  equations and measurement boundaries.
- [`docs/platform-v0.1.md`](docs/platform-v0.1.md) defines the platform and
  device contract.
- [`docs/vector-architecture.md`](docs/vector-architecture.md) defines the
  RVV contract.
- [`docs/analog-isa.md`](docs/analog-isa.md) defines the Golem analog
  instructions.
- [`docs/architecture.md`](docs/architecture.md) provides the lower-level
  implementation and data-path description.
- [`research-goals.md`](research-goals.md) records the simulator-credibility and
  architectural-research goals.
