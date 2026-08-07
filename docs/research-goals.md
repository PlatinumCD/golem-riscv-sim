# Research Goal: Analog Versus Digital Tiled Architectures

## North-star question

This project is not a paper about building a simulator. The simulator,
compiler, runtime, and hardware models are the experimental environment.

The paper asks:

> Under what architectural and workload conditions does analog computation
> provide an end-to-end system benefit over digital computation, and how must
> the compiler, memory system, network, and tile organization change as the
> bottleneck moves?

The central hypothesis is:

> Analog acceleration reduces matrix-computation pressure and increases the
> relative importance of runtime, communication, memory, and scheduling. That
> transition changes which compiler and architectural policies are optimal.

The research therefore has two connected but intentionally separate tracks:

```text
Track A: Simulator credibility
    Validate only the fidelity needed to trust an architectural experiment.
                         |
                         v
Track B: Architectural analysis
    Use the validated environment to compare analog and digital systems.
                         |
                         v
    A new architectural question exposes a missing model assumption.
                         |
                         `--------> return to Track A only as needed
```

Track A enables the paper. Track B is the paper.

---

## Track A: Simulator accuracy and credibility

### Purpose

Track A establishes that reported cycles, traffic, operations, and outputs
mean what the model says they mean. It is a bounded, demand-driven validation
effort—not an open-ended attempt to simulate every detail of a processor.

A simulator feature belongs in Track A only when it is required to answer a
Track B architectural question or to eliminate a material source of error.

### Stopping rule

The simulator is ready for a given experiment when:

1. The components exercised by that experiment have controlled validation
   tests.
2. Measured timing agrees with the model's closed-form expectation within a
   declared tolerance.
3. Functional outputs are stable across diagnostic and non-diagnostic builds.
4. Parameters classified as host-only do not change simulated results;
   temporal-lookahead parameters are recorded and pass a convergence study.
5. The model's abstractions and omissions are explicitly recorded.

It is not necessary to model unrelated hardware behavior before running the
experiment.

### Completed synchronization milestone

The QEMU/SST transmit path has been validated and repaired. The controlled
two-tile fanout study exposed instruction-quantum-amplified polling. The
implemented design adds:

- a race-safe `TX_WAIT` operation;
- a zero-wait fd-41 descriptor doorbell for every accepted deployment burst;
- exact SST timestamps for descriptor visibility;
- bounded-ring backpressure without guest busy polling;
- transmit-blocked and queue-pressure instrumentation; and
- trace-versus-summary numerical equivalence.

For 24 routes, the original one-million-instruction-quantum result fell from
2.528505 ms to 214.252 us and became invariant across 1M, 100K, and 10K
instruction quanta. After the subsequent tail-completion correction described
below, the final 24-route time is 214.763 us.

On the four-token GPT-2 deployment, synchronization repair reduced simulated
completion from 72.744181 ms to 24.026442 ms without changing:

- 1,597 tasks;
- 1,186 routes;
- 773,930 injected 32-bit words;
- 1,147,447 directional word-hops; or
- the numerical output signature.

This 3.028x reduction is a **simulator-correctness improvement**, not an
analog architectural speedup.

Those absolute times belong to the frozen Epoch C network configuration,
which was later found to use a coarse 4,096-word timing cell. The
synchronization conclusion remains valid—traffic, output, and
instruction-quantum invariance were controlled—but Epoch C's absolute
model-level makespan is historical rather than the current physical-word
baseline.

The conservative asynchronous transmit policy completed at 24.026443 ms,
only one picosecond after blocking. This establishes that descriptor capacity
and transmit/task overlap are not current bottlenecks for this GPT-2 mapping.

The complete study is in
[`docs/research-results/transmit-synchronization/`](docs/research-results/transmit-synchronization/README.md).

### Completed network timing milestone

The mesh data path now has an SST-only component test that excludes QEMU,
runtime, receive-DMA, and application work. Its first run exposed that Merlin
correctly reserves bandwidth for a full packet but notifies its endpoint when
the head arrives. Mittens previously made the complete host-side payload
visible at that callback. The tile now waits for the packet's remaining
physical beats before publishing it to fd 42 and serializes completions
through one destination-link next-available timestamp so packets cannot
overtake at the endpoint.

For the controlled 32-bit, 1 GHz, 10 ns-link mesh, 21/21 head and completion
timestamps matched the exact declared law:

```text
head = 35 + 12 * (Manhattan hops - 1)
completion = head + payload words - 1
```

The trials cover four packet sizes, one/two/four-hop routes, and one/two/four
simultaneous sources competing for a destination output. Each earlier
equal-size contender adds exactly one packet serialization interval. The
QEMU-backed pair, four-hop route, and bounded 24-destination fanout regressions
also pass after the correction. A 61-active-core GPT-2 Greedy-L3
analog/digital pair additionally validates variable-sized receive-DMA traffic:
both backends finish with unchanged numerical signatures.

The active network timing tests are the maintained evidence for this claim.

### Completed analog timing milestone

The complete custom-instruction path now has a focused QEMU/SST timing gate.
Bare-metal guests issue `set`, `load`, `execute`, and `store` through QEMU,
fd 43, and the fd 41 synchronization boundary. SST trace mode timestamps only
the modeled array queue, shared-link, and compute-engine service phases; guest
preparation and instruction time are not folded into those intervals.

An independent Python scheduler consumes the command-arrival timestamps at
the QEMU/SST boundary and predicts every following phase. Both controlled
cases matched exactly:

| Case | Active cycles, predicted/measured | Link beats, predicted/measured | Transfer overlap | Compute overlap |
|---|---:|---:|---:|---:|
| One 9x9 array | 23 / 23 | 15 / 15 | 0 | 0 |
| Two 9x9 arrays | 37 / 37 | 30 / 30 | 14 cycles | 2 cycles |

The dual-array case establishes the intended distinction: transfer demand can
overlap but only one 256-bit beat crosses the tile-wide half-duplex link each
cycle, while separate array compute engines may advance concurrently. Both
guests also verified their numerical matrix-vector outputs.

The active analog timing tests are the maintained evidence for this claim.

### Current timing and profiling capabilities

The environment currently records:

- total and vector retired instructions;
- issue-width-derived CPU cycles;
- task start and finish timestamps;
- instructions and CPU cycles inside task bodies;
- injected words and directional word-hops;
- per-packet network transit and endpoint queue time;
- router stalls;
- receive-DMA transfers and active cycles;
- analog operation type, array, transfer volume, and active cycles;
- synchronization waits by reason;
- transmit backpressure, retries, and maximum queue occupancy;
- memory requests, responses, reads, writes, and initialization traffic; and
- exact functional output signatures.

The CPU, mesh/router, analog path, transmit synchronization, and controlled
private-L1 behavior required by Epoch D now have independent validation.
Epoch D corrects the model-level deployment timing cell to one 32-bit word
while preserving 64 KiB of router buffering. Boot/programming and future
detailed-memory studies remain separate evidence gates.

### Required component-validation suite

| Component | Controlled validation | Acceptance target | Architectural studies enabled |
|---|---|---|---|
| CPU | **Complete:** fixed scalar/RVV sequences at issue widths 1, 2, and 4 and quanta 37/1,000 | **Pass:** 18/18 exact retired-count/cycle comparisons and host-quantum invariance | Digital baseline, issue-width sensitivity |
| Network | **Complete:** 1/8/64/512 words over 1/2/4-hop paths | **Pass:** 21/21 exact head and completion timestamps | Communication scaling |
| Router | **Complete:** 1/2/4-source one-hop incast | **Pass:** exact arbitration order and packet-spaced throughput | Link pressure and topology studies |
| Analog path | **Complete:** set, load, execute, store, and two-array overlap through QEMU/fd 43/fd 41/SST | **Pass:** exact phase timestamps, active cycles, link beats, serialization, and compute overlap | Analog latency and array-count sweeps |
| Native memory | **Complete:** functional reference and instruction accounting | **Pass:** stable outputs and repeatable zero-memory-delay boundary | Compute/network isolation |
| memHierarchy | **Complete:** L1 hit, capacity, conflict, eviction, LRU, and write tests | **Pass:** exact access stream, hits, misses, and completion cycles | Controlled private-L1 studies |
| Boot/programming | Known matrix volume and initialization traffic | Expected byte and cycle accounting | Cold-start versus steady-state |

Each validation should emit:

```text
component
configuration
predicted_cycles
measured_cycles
absolute_error
percentage_error
pass_or_fail
```

### Memory-backend maturity

The two memory modes serve different purposes:

| Backend | Current role | Limitation |
|---|---|---|
| `native` | Functional baseline that isolates CPU, analog, and network behavior | Does not model cache or backing-memory stalls |
| `memhierarchy` | Validated tile-private L1 timing model using SST StandardMem | Effectively one blocking post-boot memory request per core |

The current memHierarchy path cleanly separates QEMU-owned functional bytes
from SST-owned timing and cache state. Controlled tests now validate hits,
misses, conflict and capacity evictions, writes, LRU replacement, and exact
response time. Its blocking request policy remains an explicit architectural
assumption until outstanding accesses or a characterized nonblocking policy
is implemented.

Scratchpads, instruction caches, TLBs, shared lower-level caches, detailed
DRAM scheduling, and DMA/cache coherence should be added only when a Track B
experiment requires them.

### Simulator claims we can and cannot make

We can currently claim:

- end-to-end functional execution from PyTorch to per-tile RISC-V ELFs;
- deterministic QEMU/SST device-boundary synchronization;
- exact behavior of the declared scalar/RVV throughput abstraction;
- instruction-quantum-independent deployment transmission;
- controlled, exact closed-form mesh serialization, routing, and
  equal-size incast contention, plus modeled receive DMA;
- exact controlled private-L1 hit/miss and replacement timing;
- measured task, runtime, network, and analog activity; and
- repeatable numerical outputs for the validated deployments.

We cannot yet claim:

- cycle-accurate silicon performance;
- a validated superscalar CPU pipeline;
- physically validated analog latency or accuracy;
- realistic full-system memory timing;
- energy or area accuracy; or
- that every unvalidated timing parameter corresponds to fabricated hardware.

These are model boundaries, not the paper's central story.

---

## Track B: ASPLOS-style architectural analysis

### Primary contribution

The architectural contribution is a rigorous analog-versus-digital systems
analysis across compiler placement, network behavior, memory behavior, tile
provisioning, workload structure, physical nonidealities, and energy.

The paper should explain not merely whether analog is faster, but:

- where its benefit comes from;
- when that benefit disappears;
- which bottleneck replaces digital matrix computation;
- which compiler policy should respond to that transition; and
- how the tiled architecture should be provisioned as a result.

### Research question 1: When does analog beat digital?

The first requirement is a fair backend/scheduler comparison:

```text
                              Schedule
                      analog-native   digital-native
                  +-----------------+-----------------+
Backend  analog   |        A        |        B        |
         digital  |        C        |        D        |
                  +-----------------+-----------------+
```

This separates:

- backend advantage;
- scheduler advantage;
- backend-scheduler interaction; and
- performance lost by using a schedule optimized for the wrong cost model.

The digital baseline must be scheduled with digital costs known before
placement. Lowering an analog-scheduled graph to digital is a useful
cross-evaluation point, not a sufficient digital baseline.

Relevant baselines include:

- scalar RISC-V;
- RISC-V Vector;
- digital MVM or DIMC;
- ideal native analog; and
- CrossSim-backed analog.

All comparisons must use consistent precision, topology, capacity, workload,
and output-correctness requirements.

### Research question 2: Where does the bottleneck move?

Every representative execution should decompose its critical path into:

```text
Total latency
├── compiled digital task execution
├── analog operations
│   ├── matrix programming
│   ├── input loading
│   ├── array execution
│   └── result storage
├── runtime and dispatch
├── memory stalls
├── network serialization
├── network contention and queueing
├── receive DMA
├── dependency waiting
└── idle time and load imbalance
```

The following four-token GPT-2 critical-path decomposition is an Epoch C
historical measurement and must be regenerated under the current Epoch E
configuration before entering the paper:

| Category | Time | Fraction |
|---|---:|---:|
| Task execution | 14.319 ms | 59.6% |
| Network and receive DMA | 4.352 ms | 18.1% |
| Destination waiting | 0.102 ms | 0.4% |
| Source dispatch/runtime | 2.829 ms | 11.8% |
| Same-tile gaps | 2.199 ms | 9.2% |

The traced run retires 62,536,306 instructions:

- 29,857,225 inside compiled task bodies; and
- 32,679,081 in boot, runtime, communication, and other out-of-task code.

The 52.26% out-of-task instruction fraction is not equivalent to 52.26% of
time waiting for communication. Instructions execute across tiles and may
overlap; critical-path attribution is the timing authority.

### Research question 3: Where are the analog phase boundaries?

This should become the paper's signature experiment.

Sweep:

- digital computation cost;
- analog execution latency;
- array programming cost;
- activation and weight movement;
- memory latency and bandwidth;
- network load and hop count;
- array count and dimensions;
- mesh scale;
- active execution count; and
- compiler scheduling policy.

Construct a phase diagram:

```text
                         Increasing communication pressure ->
              +----------------------+--------------------------+
High digital  | Analog wins;         | Analog wins only with    |
work          | compute-oriented     | network-aware scheduling |
              | placement suffices   |                          |
              +----------------------+--------------------------+
Low digital   | Placement has        | Communication or memory  |
work          | limited influence    | erases analog benefit    |
              +----------------------+--------------------------+
                  Increasing analog compute advantage
```

The plot must identify:

- which backend wins;
- which scheduler wins;
- whether the system is compute-, network-, memory-, or runtime-bound;
- the saturation point; and
- where the optimal policy changes.

This is the central architectural claim: treating analog arrays as simple
faster substitutes for digital matrix multiplication can optimize the wrong
system bottleneck.

### Research question 4: How should tiles and meshes be provisioned?

Study:

- mesh sizes from 2x2 through at least 12x12;
- active versus physically available tiles;
- arrays per tile;
- array rows and columns;
- shared versus per-array analog links;
- scalar issue width;
- RVV support;
- L1 capacity and latency;
- router buffering and latency; and
- the fixed 32-bit mesh baseline plus controlled width sensitivity.

Report:

- speedup and absolute latency;
- useful tile and array utilization;
- injected words and word-hops;
- per-link utilization and hotspot maps;
- load imbalance;
- memory pressure;
- saturation point; and
- performance lost by adding hardware beyond the useful region.

### Research question 5: How general is the result?

The workload suite should represent different computational and communication
structures:

- GPT-2 or another decoder transformer;
- BERT or another encoder transformer;
- ResNet-18/50;
- Vision Transformer;
- a pure MLP;
- an embedding- or recommendation-heavy model; and
- synthetic pipeline, fanout, reduction, and hotspot graphs.

The purpose is not merely to accumulate model names. Each workload must test a
specific hypothesis about computation, communication, memory, or scheduling.

### Research question 6: What changes under sustained execution?

Study both cold-start latency and steady-state throughput:

- one cold inference;
- one warm inference with resident weights;
- batches and concurrent executions of 2, 4, 8, 16, and 32;
- arrival-rate sweeps;
- model-switching and reprogramming cost;
- buffer occupancy and backpressure;
- head-of-line blocking; and
- p50, p95, and p99 latency.

This determines whether the architecture is best understood as a single-shot
accelerator, a throughput engine, or both.

### Research question 7: Does the advantage survive physical constraints?

CrossSim studies should vary:

- ADC and DAC precision;
- conductance/programming variation;
- read noise;
- drift and retention;
- IR drop and parasitic resistance;
- weight and activation quantization; and
- repeated device seeds.

Report application quality, not only tensor differences:

- GPT-2 perplexity or logit deviation;
- ResNet top-1/top-5 accuracy; and
- classification agreement where appropriate.

The final analysis should connect accuracy to latency and energy.

### Research question 8: What are the energy and area consequences?

Instrument and characterize:

- CPU instructions;
- cache, SRAM, and backing-memory accesses;
- router traversals and link transfers;
- DAC and ADC operations;
- array programming and execution;
- weight and activation movement; and
- off-chip memory.

Report:

- joules per inference;
- energy-delay product;
- throughput per watt;
- performance per square millimeter;
- component energy and area breakdown; and
- cold-start versus amortized programming energy.

Energy must include the surrounding digital system. Peak analog-array
efficiency alone is not a system-level comparison.

---

## Current architectural evidence

The current results are a strong pilot, not final-paper evidence.

| Potential claim | Current evidence | Status |
|---|---|---|
| PyTorch models execute through the complete stack | Torch-MLIR, Sculptor, per-tile ELF, runtime, QEMU, SST | Strong |
| Deployment transmission is synchronized correctly | Fanout sweep, quantum invariance, exact output | Strong |
| Analog reduces digital instruction work | Existing GPT-2 analog/digital measurements | Strong for the tested graph |
| Scheduler quality depends on backend | Analog and digital winners differ in the original sweep | Compelling hypothesis; corrected rerun required |
| Communication minimization always improves performance | Original sweep contains counterexamples | Valuable preliminary negative result |
| Analog is faster end to end | Epoch D Greedy-Timing-L3 pilot reports 9.84x for one backend-specific pair | Preliminary; fair corrected sweep required |
| Communication dominates corrected GPT-2 | Direct communication is approximately 18.5% of the measured critical path | Not supported |
| Runtime/NIC overlap is a major limiter | Async and blocking differ by one picosecond | Not supported for this mapping |
| CPU throughput abstraction is internally consistent | Exact scalar/RVV issue-width tests at two host quanta | Strong for the declared throughput model |
| NIC/router timing follows the declared link model | 21 exact serialization, distance, and contention observations | Strong |
| Analog timing follows the declared shared-link model | End-to-end single/dual-array command timelines | Strong |
| Private-L1 timing follows its declared model | Exact hit/miss, conflict, LRU, capacity, and response latency | Strong for controlled private-L1 studies |
| Analog remains beneficial with nonideal devices | CrossSim path exists without complete accuracy study | Unsupported |
| Analog is more energy efficient | No calibrated whole-system energy model | Unsupported |
| Results generalize across workloads and scales | Limited workload and architecture coverage | Unsupported |

### Evidence epochs

Results must identify the simulator/compiler epoch that produced them.

#### Epoch A: pre-synchronization pilot

The original 112 GPT-2 scheduler trials generated the scheduler/backend
interaction hypothesis. They should guide experiment design, but their
absolute timing and speedup values are not final evidence.

#### Epoch B: corrected synchronization

The fanout study and corrected four-token GPT-2 execution validate the
transmit synchronization model and establish the current 24.026442 ms
baseline.

#### Epoch C: first component-validated baseline

Epoch C is frozen for historical reproduction. CPU, NIC/router, analog,
private-L1, and transmit-synchronization gates met their declared acceptance
criteria, but the model-level deployments used a 4,096-word timing cell while
the isolated physical-word network validation used one-word cells. Epoch C
absolute deployment timing is therefore superseded.

#### Epoch D: corrected physical-word network baseline

Epoch D is frozen as of 2026-07-30. It retains Epoch C's CPU, analog, memory,
runtime, and synchronization models but uses one 32-bit timing cell and
16,384 such cells of buffering, preserving the previous 64 KiB capacity.
The exact network gate passes 21/21 observations, the deployment-runtime and
producer–MVM–recombine regressions pass, and a focused GPT-2 analog/digital
pair completes correctly. A receive-side audit also shows that the fixed
one-million-instruction lookahead is 523 ns (1.45%) above the converged
1,000/100-instruction result in a 24-route fanout. Epoch D therefore validates
the physical-word network correction but does not yet close the receive-side
co-simulation convergence gate. The machine-readable configuration is
`config/epoch-d.env`.

#### Epoch E: 100 ns analog-MVM baseline

Epoch E retains Epoch D's physical-word network, CPU, memory, receive-DMA, and
synchronization configuration. It raises the compiler placement cost and SST
execution delay of one analog MVM from 8 ns to 100 ns. The historical
eight-cycle analog timing test remains a mechanism-validation fixture rather
than evidence for the new physical latency. Epoch E's machine-readable
configuration is `config/epoch-e.env`.

---

## Experimental discipline

### Evidence gate

A result may enter the final paper only when:

- its exercised timing models have passed component validation;
- its compiler, simulator, workload, and configuration versions are recorded;
- its functional output meets the required correctness criterion;
- trace and non-trace output signatures agree;
- parameters classified as host-only do not alter simulated time, and any
  temporal-lookahead setting has a preserved convergence study;
- raw profiles, manifests, and summaries are preserved; and
- the plotted metric has a documented physical or modeled interpretation.

### Repetition policy

Deterministic configurations normally require one verified execution.
Repeating an identical deterministic SST run does not add statistical
evidence.

Use repeated seeds where randomness exists:

- random scheduling: at least 30 seeds;
- CrossSim/device variation: approximately 20-50 seeds per representative
  point;
- stochastic traffic or input generation: enough seeds for confidence
  intervals; and
- accuracy studies: a complete or statistically justified dataset.

The final evaluation needs orthogonal evidence, not an indiscriminate full
factorial. Use screening sweeps to locate important parameters, then study the
resulting transition regions densely.

### Reproducibility

Every major experiment should preserve:

- exact compiler pass pipeline;
- compiler and third-party commit identities;
- simulator parameters;
- task and route manifests;
- numerical output signature;
- raw per-tile profiles;
- joined summaries;
- analysis scripts;
- plotted source data; and
- checksums.

The transmit-synchronization study provides the first packaged example at
`build/research-artifacts/transmit-synchronization.tar.gz`.

---

## Roadmap

### Completed foundation

- End-to-end PyTorch-to-mesh execution.
- Per-tile RISC-V ELF generation.
- Bare-metal task and boot registries.
- Custom Golem analog ISA in QEMU.
- SST mesh, NIC, analog, and optional memory bridges.
- Native and CrossSim analog backends.
- Source-aware routed 32-bit tensor transfers.
- Receive DMA.
- Transmit synchronization repair and validation.
- Task, network, analog, memory, and wait profiling.
- Exact scalar/RVV issue-width and host-quantum validation.
- Exact link serialization, distance, and contention validation.
- End-to-end shared-link analog timing validation.
- Exact private-L1 hit/miss, conflict, LRU, and capacity validation.
- Frozen Epoch C historical configuration, frozen Epoch D physical-word
  network baseline, and current Epoch E 100 ns analog-MVM baseline.

### Completed: validate the experimental instrument

1. Scalar/RVV instruction and issue-width timing: complete.
2. One-hop, multi-hop, sustained-throughput, and contention timing: complete.
3. Analog set/load/execute/store and shared-link timing: complete.
4. Private-L1 behavior required by the first memory study: complete.
5. Frozen validated simulator/compiler configuration as Epoch C: complete.
6. Corrected model-level physical-word network configuration as Epoch D:
   complete; receive-side temporal-lookahead convergence remains open.
7. Matched 100 ns compiler and SST analog-MVM configuration as Epoch E:
   complete; end-to-end experiments must be regenerated.

### Medium term: establish the architectural result

1. Make receive-state observation event exact or select and justify a
   quantum-converged lookahead bound.
2. Rerun the GPT-2 scheduler sweep under Epoch E.
3. Implement and run the fair analog/digital backend-scheduler matrix.
4. Produce exact critical-path and resource-utilization comparisons.
5. Identify the first compute/network/memory transition boundaries.
6. Construct the initial analog-versus-digital phase diagram.
7. Validate scheduler cost estimates against measured SST makespan.
8. Compare small graphs against exhaustive optimal placement.

### Long term: establish generality and physical relevance

1. Expand across transformer, CNN, MLP, vision, and embedding workloads.
2. Perform mesh, array, issue-width, memory, and topology scaling.
3. Measure cold-start, warm execution, concurrency, and throughput.
4. Add CrossSim nonideality and application-accuracy studies.
5. Add calibrated whole-system energy and area.
6. Produce accuracy-latency-energy Pareto frontiers.
7. Test scheduler generalization on unseen workloads.

---

## Final-paper figure plan

### Main architectural figures

1. Full compiler-to-tiled-architecture overview.
2. Fair backend-scheduler interaction matrix.
3. Analog versus digital critical-path breakdown.
4. Scheduler rank reversal and regret.
5. Compute/network/memory phase diagram.
6. Per-link utilization and hotspot maps.
7. Mesh and array scaling with saturation.
8. Cold-start versus steady-state throughput.
9. CrossSim accuracy-latency tradeoff.
10. Whole-system energy and area breakdown.
11. Accuracy-latency-energy Pareto frontier.
12. Workload-wide architectural summary.

### Methodology and validation figures

1. Component predicted-versus-measured timing error.
2. QEMU/SST synchronization validation.
3. Instruction-quantum invariance.
4. Native versus memHierarchy cache microbenchmarks.
5. Reproducibility and evidence-flow diagram.

The validation figures support confidence in the instrument. They should not
displace the analog-versus-digital architectural results from the center of
the paper.

---

## Paper thesis

The intended conclusion is not:

> We built a simulator, or analog is some fixed amount faster.

It is:

> Analog acceleration reorganizes the system's bottlenecks. Its end-to-end
> value depends on compiler placement, network pressure, memory behavior,
> tile provisioning, workload structure, device accuracy, and energy. A
> backend-aware compiler and co-designed tiled architecture are necessary to
> capture that value across the regimes where analog computation is useful.

That is the research-center thesis.

## Relevant methodological references

- [PUMA: A Programmable Ultra-efficient Memristor-based
  Accelerator](https://arxiv.org/abs/1901.10351)
- [Analog versus digital in-memory-computing system
  modeling](https://arxiv.org/abs/2405.14978)
- [CrossSim](https://www.sandia.gov/ccr/software/crosssim/)
- [Analog inference accuracy study](https://arxiv.org/abs/2109.01262)
- [SCALE-Sim v3](https://arxiv.org/abs/2504.15377)
- [Heterogeneous-system energy
  modeling](https://accelergy.mit.edu/ispass2021/hetergeneous_computing_abstract.pdf)
