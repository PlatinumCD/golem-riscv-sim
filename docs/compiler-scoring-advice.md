# Advice for Compiler

## Purpose

This document specifies the compiler changes needed to make Sculptor's placement, scheduling, and timing scores reflect the behavior observed in the Golem QEMU + SST simulator.

The immediate problem is not that the compiler prefers one imperfect schedule over another. The problem is that several compiler metrics are being interpreted as elapsed-time contributions even though they are aggregate graph costs or spatial heuristics. This produces conclusions such as "communication is 54% of runtime" or "contention is 44% of runtime" that are not supported by the measured execution.

This specification separates four concepts that must no longer be conflated:

1. **A placement heuristic** used to guide a search.
2. **Aggregate work** performed across every task or route.
3. **Predicted elapsed time** for the whole deployment.
4. **Exposed delay** that actually extends the deployment makespan.

The intended audience is the Sculptor compiler engineer. The terms **MUST**, **SHOULD**, and **MAY** are normative.

---

## Executive finding

The current compiler score substantially underestimates digital task time and overstates the elapsed-time importance of communication contention.

For the last directly comparable GPT-2 analog 12x12 balanced deployment:

| Quantity | Value |
|---|---:|
| SST measured makespan | 12.013709 ms |
| Compiler predicted critical path | 0.856095 ms |
| Compiler-to-SST makespan error | 14.03x underprediction |
| Measured critical-chain task execution | 10.106004 ms |
| Measured critical-chain task execution share | 84.1% |
| Measured same-core/runtime gaps | 1.157760 ms |
| Measured same-core/runtime gap share | 9.64% |
| Measured exposed route-boundary gaps | 0.511103 ms |
| Measured exposed route-boundary share | 4.25% |
| Boot-to-first-critical-task interval | 0.236785 ms |

The compiler reports:

| Compiler aggregate | Value |
|---|---:|
| Predicted critical path | 0.856095 ms |
| Sum of network latency across edges | 3.728670 ms |
| Sum of network contention delay across edges | 2.770476 ms |

The compiler's summed network latency is 4.36 times larger than its own predicted makespan, and its summed contention delay is 3.24 times larger than that makespan. Those values therefore cannot be valid percentages of elapsed runtime.

The central conclusion is:

> The existing network totals are useful aggregate pressure statistics, but they are not elapsed-time decomposition terms. The existing digital cost model is also too optimistic to drive a reliable makespan prediction.

Both problems must be corrected. Merely changing the scheduler's communication weight will not fix the model.

---

## Evidence baseline

The findings in this document are based on the following artifacts:

- Sculptor source revision: `cdfcc7a`
- Compiler IR:
  `build/research/gpt2-analog-12x12-cdfcc7a/balanced/lowering/partitioned.mlir`
- SST performance summary:
  `build/research/gpt2-analog-12x12-cdfcc7a/balanced/deployment/performance-profile/summary.json`
- SST critical path:
  `build/research/gpt2-analog-12x12-cdfcc7a/balanced/deployment/performance-profile/critical-path.csv`
- SST route trace:
  `build/research/gpt2-analog-12x12-cdfcc7a/balanced/deployment/performance-profile/routes.csv`

That execution contained:

| Measured property | Value |
|---|---:|
| Active tiles | 66 |
| Runtime tasks | 1,837 |
| Routes | 1,382 |
| Total retired instructions | 53,683,072 |
| Task retired instructions | 20,552,950 |
| Total CPU cycles | 26,844,563 |
| Task CPU cycles | 10,277,760 |
| Retired vector instructions | 8,717,929 |
| Analog operations | 3,120 |
| Analog set operations | 240 |
| Analog load operations | 960 |
| Analog compute operations | 960 |
| Analog store operations | 960 |
| Injected 32-bit words | 937,726 |
| Routed word-hops | 1,752,108 |
| Network stall events/cycles reported | 80,643 |
| Receive-DMA cycles | 127,408 |

### Important MVM-latency note

The evidence above was recorded when the analog MVM latency was 8 ns. The simulator default has since been changed to 100 ns.

This does not invalidate the diagnosis:

- The erroneous aggregate-versus-exposed interpretation is independent of MVM latency.
- Core serialization and NIC injection semantics are independent of MVM latency.
- The dominant mixed fused digital tasks remain underestimated by approximately 30x even after changing their analog execution component from 8 ns to 100 ns.

The complete validation suite MUST be rerun using 100 ns after the compiler corrections are implemented. The old trace should remain as a frozen regression fixture.

---

## Scope

This work MUST:

- Correct the meaning and naming of timing metrics.
- Predict per-task execution time using a model consistent with the declared QEMU/SST architecture.
- Model serialization of work on each tile.
- Model source NIC injection and directed-link reservations.
- Distinguish aggregate network work from exposed network delay.
- Produce a causal critical path rather than a set of zero-slack nodes.
- Re-score complete placements, including reductions and other digital-only islands.
- Preserve the existing 32-bit routed-word architecture.
- Preserve deterministic XY routing unless a separate routing change is explicitly requested.
- Retain enough provenance to reproduce every timing estimate.

This work MUST NOT:

- Change the Golem ISA to make a score look better.
- Change the physical link width merely to calibrate the compiler to one trace.
- Treat host wall-clock execution time as simulated target time.
- Change task-graph correctness or route identity semantics.
- Deduplicate routes unless runtime readiness semantics are changed deliberately.
- Claim that the current native-memory backend models a realistic cache or DRAM hierarchy.
- Tune a single global multiplier against GPT-2 and call the result a general timing model.

---

## Required terminology

The implementation and reports MUST use the following meanings consistently.

### Makespan

Elapsed simulated time from the selected start boundary to completion of the selected final output.

Two makespans SHOULD be reported:

- **Cold makespan:** includes boot, array setup, runtime readiness, and model execution.
- **Warm makespan:** starts after all boot/setup tasks complete and the tiles are ready for the first model input.

### Aggregate task work

The sum of all task execution times across all tiles. This may be much greater than makespan because tiles execute in parallel.

### Aggregate network service

The sum of ideal service time for all routes. This is a measure of network work, not elapsed runtime.

### Aggregate network queue delay

The sum of queueing delay observed by all routes. This is a measure of congestion pressure, not elapsed runtime.

### Exposed transport delay

The increase in makespan caused by finite, nonzero transport time after removing shared-resource contention.

### Exposed contention delay

The increase in makespan caused specifically by routes sharing constrained resources.

### Spatial link pressure

An order-independent placement heuristic based on multiple routes using the same links. It does not imply that those routes overlap in time.

### Causal critical path

The actual chain of task, core-queue, NIC-queue, route, and receive events that determines the final completion time.

### Compiler score

An optimization objective. A compiler score MUST NOT be labeled as nanoseconds, cycles, or a runtime percentage unless it is produced by a timing model with those semantics.

---

## Diagnosis of the current implementation

### 1. Digital operations are converted to cycles incorrectly

Current logic in:

`lib/Dialect/Sculptor/Transforms/task_timing/TaskLatencyModel.cpp`

effectively computes:

```text
vector_ops_per_cycle = digital_vector_bits_per_cycle / 32
operations_per_cycle = max(digital_issue_width, vector_ops_per_cycle)
digital_cycles = ceil(digital_ops / operations_per_cycle)
```

For a 256-bit vector width and 32-bit elements, this treats all recorded digital operations as though eight scalar operations complete every cycle.

That assumption is not valid for the generated runtime:

- Not every operation becomes a vector arithmetic instruction.
- Address generation consumes instructions.
- Loop induction and branch instructions consume instructions.
- Loads and stores consume instructions.
- Memref descriptor operations consume instructions.
- Task dispatch and readiness handling consume instructions.
- Tensor extraction, copying, packing, and insertion are not free.
- A QEMU issue-width timing model charges retired target instructions, not high-level MLIR arithmetic operations.

#### Measured example

A repeated `mixed.fused` task in the GPT-2 deployment has compiler metadata approximately equivalent to:

```text
digital_ops          = 122,880
analog_load_ns       = 256
analog_execute_ns    = 32
analog_store_ns      = 512
intrinsic_latency_ns = 16,160
```

The corresponding SST task instances take approximately:

```text
522.959 microseconds
1,044,884 retired instructions
522,447 CPU cycles
```

The compiler prediction is therefore about 32.36x too small for that task family. Twelve repetitions contribute approximately 6.276 ms, or 52.2% of the measured full execution.

Changing MVM latency to 100 ns only increases the four-array analog execution portion by hundreds of nanoseconds. It does not explain the approximately 507 microsecond missing digital/runtime cost.

#### Required correction

The compiler MUST stop dividing every high-level digital operation by the vector-lane count.

The compiler MUST instead estimate dynamic target instruction classes or an equivalent calibrated task cost.

At minimum, each task cost SHOULD distinguish:

```text
scalar_compute_instructions
vector_compute_instructions
load_instructions
store_instructions
branch_and_control_instructions
runtime_dispatch_cycles
analog_load_cycles
analog_execute_cycles
analog_store_cycles
```

For the current simplified QEMU timing contract, an acceptable first formula is:

```text
cpu_instructions =
    scalar_compute_instructions
  + vector_compute_instructions
  + load_instructions
  + store_instructions
  + branch_and_control_instructions

cpu_cycles =
    ceil(cpu_instructions / issue_width)
  + fixed_runtime_dispatch_cycles
  + task_specific_runtime_cycles
```

If QEMU later gains a more realistic issue/resource model, the compiler can evolve to:

```text
frontend_cycles = ceil(dynamic_instructions / issue_width)
vector_cycles   = ceil(vector_lane_operations / vector_lanes)
memory_cycles   = ceil(local_memory_bytes / local_memory_bytes_per_cycle)

cpu_cycles =
    max(frontend_cycles, vector_cycles, memory_cycles)
  + dependency_latency_cycles
  + branch_penalty_cycles
  + runtime_overhead_cycles
```

The chosen formula MUST be versioned and tied to the simulator configuration.

---

### 2. Some nontrivial tasks are effectively costed as zero

Several tensor movement and control routines can contain loops, loads, stores, index calculations, and descriptor manipulation while carrying `digital_ops = 0`.

Examples include task families such as:

- token extraction
- vector tiling
- tensor slicing
- tensor insertion
- packing/unpacking
- route-buffer copy tasks
- reductions whose arithmetic count omits movement overhead

This causes the compiler to place and schedule substantial digital work as if it were free.

#### Required correction

A task with a nonempty executable body MUST receive a cost.

The compiler SHOULD inspect the final task function body after all relevant fusion and lowering transformations and derive:

- Static loop trip counts where possible.
- Dynamic instruction estimates for loads, stores, arithmetic, vector operations, branches, calls, and descriptor work.
- Bytes moved between local runtime buffers.
- Fixed task-entry and task-exit overhead.

If a task is marked zero-cost but its callee contains executable operations, the compiler MUST emit either:

1. a hard diagnostic in timing-validation mode; or
2. a warning plus an explicit conservative fallback cost.

Silently treating it as free is not acceptable.

---

### 3. Fused tasks inherit stale timing instead of being re-costed

Current behavior in:

- `TaskGraphRoutineFuser.cpp`
- `TaskLatencyModel.cpp`

allows a fused task to inherit sums of timing attributes from its pre-fusion components. `mixed.fused` may then return those stored attributes without analyzing the final fused function.

Fusion changes:

- Loop structure.
- Intermediate tensor materialization.
- Data reuse.
- Instruction count.
- Address generation.
- Function-call boundaries.
- Analog operation ordering.
- Opportunities for vectorization.

The cost of the fused function is not necessarily the sum of the old costs.

#### Required correction

After fusion, the compiler MUST invalidate pre-fusion derived timing attributes.

The final fused function MUST be analyzed again.

One of these mechanisms SHOULD be used:

- Remove all derived timing attributes from a newly fused task and regenerate them.
- Attach `sculptor.timing.generation` and reject timing data whose generation predates the latest structural rewrite.
- Separate semantic workload metadata from derived timing metadata, preserving only the former through fusion.

The third option is preferred:

```text
Semantic metadata:
  tensor shapes
  operation kinds
  bytes moved
  array bindings
  analog execution counts

Derived timing:
  instruction estimate
  task cycles
  queue delay
  start and finish timestamps
```

Structural passes MAY preserve semantic metadata. They MUST invalidate derived timing.

---

### 4. The post-placement timing model does not serialize tasks on a core

Current placement-aware timing in:

`TaskGraphNetworkTiming.cpp`

allows every zero-dependency task to begin at time zero and starts a task as soon as its graph inputs arrive. It does not maintain a per-core `busyUntil` time or runtime-ready queue.

This permits multiple tasks assigned to one RISC-V tile to execute concurrently in the compiler model.

The actual runtime does not behave this way. In:

`third_party/sculptor-mlir/runtime/src/deployment_runtime.cpp`

the runtime scans tasks in local runtime-index order, selects a ready task, and executes one task on the tile. Output transmission may overlap with later ready work under the configured policy, subject to output-buffer conflicts, but two CPU task bodies do not execute simultaneously on one core.

The current Sculptor placement documentation already notes that processor execution and several hardware resources are not serialized. This limitation now materially invalidates the score.

#### Required correction

The timing simulator MUST represent each active tile as a stateful resource:

```text
CoreState:
  available_time
  ready_queue
  running_task
  local_runtime_order
```

When a task's dependencies are satisfied:

1. Enqueue it on its assigned core.
2. If the core is idle, select the next task using the runtime's actual policy.
3. Start it no earlier than both its data-ready time and the core's available time.
4. Advance the core's available time to the task's finish.
5. Release its output routes according to the runtime transmit policy.

The default ready-task ordering MUST match the generated runtime. If the runtime selects the lowest local `runtime.task_index`, the compiler model MUST do the same unless another policy is explicitly encoded in the deployment.

The event trace MUST distinguish:

- waiting for task inputs;
- waiting for the assigned core;
- executing on the core;
- waiting for output-buffer safety;
- waiting for NIC injection;
- moving through the network;
- waiting for receive completion.

---

### 5. The network totals are aggregate edge work, not runtime shares

Current logic accumulates:

```text
totalNetworkLatencyNs += transfer.latencyNs
totalNetworkContentionDelayNs += transfer.contentionDelayNs
```

for every route.

This is valid as an aggregate statistic. It is invalid as an elapsed-time decomposition because:

- Independent transfers happen in parallel.
- Transfers may be off the critical path.
- Queueing on one link may be hidden behind computation elsewhere.
- Multiple delayed routes may all feed the same later join, so summing them double counts elapsed impact.
- The aggregate can exceed total makespan, as it does in the observed GPT-2 result.

#### Required correction

Rename the metrics:

```text
totalNetworkLatencyNs
  -> sumEdgeNetworkServiceNs

totalNetworkContentionDelayNs
  -> sumEdgeNetworkQueueDelayNs
```

Reports MUST label them as aggregate network work and aggregate queue pressure.

They MUST NOT:

- be divided by makespan to produce a runtime percentage;
- be displayed as stacked portions of runtime;
- be called "time spent communicating" without the word "aggregate";
- be used as proof that network delay dominates execution.

Elapsed contribution MUST be computed through critical-event attribution or counterfactual replay, described below.

---

### 6. Incoming network delay is not causally attributed

The current definition is approximately:

```text
incoming_network_delay =
    task_earliest_start
  - latest_predecessor_finish
```

The predecessor with the latest finish is not necessarily the predecessor whose route arrives last. Another predecessor may finish earlier but traverse a longer or congested route.

This can attribute the wrong interval to network delay and identify the wrong critical predecessor.

#### Required correction

For every task input, record:

```text
producer_finish_time
route_injection_start
route_arrival_time
receive_completion_time
consumer_ready_time
```

The causal predecessor for task readiness is the input event with the latest receive-completion time, not automatically the producer with the latest task-finish time.

If the task starts later because its core is occupied, the causal predecessor becomes the previous task on that core rather than the incoming route.

---

### 7. Zero-slack tasks are not necessarily one causal critical chain

Current reverse timing analysis can mark multiple tasks as critical when they have zero slack. In a graph with joins, shared resources, and contention, this may identify several parallel branches. Summing all marked task or communication costs can double count time.

#### Required correction

The forward event simulation MUST record a single causal parent for every state transition that establishes a later start time:

```text
Task start parent:
  latest required input completion
  OR previous task on the same core

Route injection parent:
  producer completion
  OR source NIC availability

Link entry parent:
  previous hop arrival
  OR previous reservation on that directed link

Receive completion parent:
  final-link arrival
  OR destination receive-engine availability
```

Starting from the event that establishes model-output completion, the report generator SHOULD walk those parent pointers backward to produce one causal chain.

Ties MAY retain multiple parents, but timing components MUST not be summed twice. If multiple parents are retained, the output must be represented as a critical DAG rather than a single summed list.

---

### 8. Link pressure is spatial overlap, not temporal contention

Current greedy link-pressure logic charges routes when they share directed links using a quantity related to:

```text
new_edge_bytes * already_assigned_edge_bytes
```

This is order-independent and does not determine whether the transfers overlap in time. Two routes that use the same link at completely different times still create a penalty.

That can be a useful locality/concentration heuristic. It is not contention.

#### Required correction

The existing heuristic SHOULD be renamed:

```text
link_pressure -> spatial_shared_link_pressure
```

It MAY remain:

- a search tie-breaker;
- a cheap lower-bound feature;
- a topology-awareness term.

It MUST NOT be reported as predicted contention or nanoseconds.

If a time-based link-pressure heuristic is desired during search, it SHOULD use estimated release windows:

```text
temporal_overlap(route_a, route_b)
  × shared_directed_links(route_a, route_b)
  × overlapping_words
```

The final schedule MUST still be evaluated by the resource event model.

---

### 9. The greedy timing objective sums communication proxies

The greedy timing search accumulates exposed-latency-like terms over graph edges. Parallel paths and later joins can cause those terms to be counted more than once.

This value is useful as a search proxy but is not an actual makespan.

#### Required correction

The search implementation MUST name proxy scores distinctly from predicted time:

```text
searchCommunicationProxy
searchCriticalityProxy
searchCoreLoadProxy
```

It SHOULD use a two-level strategy:

1. Use inexpensive admissible or monotonic proxies while expanding the beam.
2. Run the corrected timing event simulation on every complete beam candidate.
3. Rank final candidates by predicted full-deployment makespan.

If exact replay during every expansion is affordable, it MAY replace the proxy. The required result is that the final selected candidate has been evaluated using full timing semantics.

---

### 10. Digital-only and reduction placement is not fully represented in the timing search

The primary timing search is centered on analog islands. Digital-only islands and reduction placement may be completed afterward. The reported objective can therefore omit work that strongly affects the final graph, including recombination and reduction hubs.

This is especially important for the GPT-2 experiments where concentration of reduction work on a small number of cores changes the measured critical path.

#### Required correction

The final timing objective MUST include:

- analog islands;
- digital-only islands;
- balanced or unbalanced reduction tasks;
- inter-island route tasks;
- final fused tasks;
- the actual per-core task order;
- all route boundary operations.

Acceptable implementations are:

#### Preferred

Place all islands in one unified search state and score the complete deployment.

#### Acceptable intermediate

1. Search analog placement.
2. Materialize digital and reduction placement.
3. Run exact full-deployment timing.
4. Perform local refinement or rerank several analog candidates using the complete score.

The compiler MUST NOT present an analog-island-only objective as the predicted runtime of the completed deployment.

---

### 11. The network model lacks source-NIC and receive-resource behavior

Directed-link reservation is necessary but not sufficient.

A tile has one tile NIC path into the mesh under the current platform contract. It cannot inject arbitrary unrelated routes at the same instant merely because their later paths diverge.

The current runtime trace also includes protocol words and receive-DMA work.

#### Required correction

The event model SHOULD include:

```text
NicTxState per tile:
  available_time
  current_route

DirectedLinkState per direction:
  reserved intervals or next_available_time

RxDmaState per tile:
  available_time
  destination buffer
```

For each route:

```text
payload_words  = ceil(byte_size / 4)
protocol_words = configured_protocol_words
total_words    = payload_words + protocol_words
```

The current protocol overhead is five 32-bit words per logical route and SHOULD be a configurable platform parameter rather than a magic constant.

The model MUST:

- reserve source injection bandwidth;
- reserve each directed XY link;
- allow opposite directions to operate independently if the hardware does;
- include per-hop latency;
- include destination receive/DMA completion before the task input becomes ready;
- report queueing at each resource separately.

#### Backpressure fidelity

An interval-reservation model with effectively sufficient buffering is acceptable for the first corrected version. It does not need to reproduce every Merlin credit event immediately.

The report MUST identify that approximation. A later version MAY add:

- finite router buffers;
- credit round trips;
- head-of-line blocking;
- packet arbitration;
- virtual channels, if the architecture actually introduces them.

No virtual channels should be invented merely to improve results.

---

### 12. Cold boot and warm inference are currently mixed

The compiler timing documentation generally treats matrix setup as outside warm model execution. A full SST run includes:

- ELF/runtime boot;
- task registry initialization;
- analog array setup;
- ready signaling;
- inference execution.

In the measured trace, approximately 0.237 ms elapsed before the first critical model task.

#### Required correction

Every timing report MUST state its boundary:

```text
cold_start_ns
boot_and_setup_ns
warm_inference_ns
cold_makespan_ns
```

Compiler scheduling SHOULD normally optimize warm inference unless deployment explicitly requests cold-start optimization.

Compiler-versus-SST validation MUST compare like with like:

- compiler warm estimate versus SST warm interval; or
- compiler cold estimate including modeled setup versus SST full makespan.

---

## Proposed timing architecture

### A. Data model

The analysis SHOULD introduce explicit resource and event state.

```cpp
struct TaskCost {
  uint64_t scalarInstructions;
  uint64_t vectorInstructions;
  uint64_t loadInstructions;
  uint64_t storeInstructions;
  uint64_t controlInstructions;
  uint64_t runtimeDispatchCycles;
  uint64_t analogLoadCycles;
  uint64_t analogExecuteCycles;
  uint64_t analogStoreCycles;
  uint64_t estimatedLocalBytesRead;
  uint64_t estimatedLocalBytesWritten;
  StringRef modelVersion;
};

struct TaskState {
  TaskId id;
  CoreId core;
  unsigned localTaskIndex;
  unsigned unsatisfiedInputs;
  uint64_t readyTime;
  uint64_t startTime;
  uint64_t finishTime;
};

struct CoreState {
  uint64_t availableTime;
  SmallVector<TaskId> readyTasks;
  std::optional<TaskId> runningTask;
};

struct RouteState {
  RouteId id;
  CoreId sourceCore;
  CoreId destinationCore;
  uint64_t payloadWords;
  uint64_t protocolWords;
  uint64_t producerFinishTime;
  uint64_t injectionStartTime;
  uint64_t arrivalTime;
  uint64_t receiveCompleteTime;
};

struct TimingScenario {
  bool enableNetwork;
  bool enableSharedLinkContention;
  bool enableSourceNicSerialization;
  bool enableReceiveDma;
  bool includeBoot;
};

struct TimingEstimate {
  uint64_t makespanNs;
  uint64_t sumTaskWorkNs;
  uint64_t sumEdgeNetworkServiceNs;
  uint64_t sumEdgeNetworkQueueDelayNs;
  uint64_t sumCoreQueueDelayNs;
  uint64_t sumNicQueueDelayNs;
  uint64_t sumReceiveQueueDelayNs;
  SmallVector<EventId> causalCriticalChain;
};
```

Names may vary, but these semantic distinctions MUST remain.

### B. Event types

The simulator SHOULD use a deterministic priority queue containing events such as:

```text
BootComplete
DependencyArrived
TaskReady
TaskStarted
TaskFinished
RouteReady
RouteInjectionStarted
RouteLinkEntered
RouteLinkExited
RouteArrived
ReceiveDmaComplete
ModelOutputComplete
```

The implementation does not need to enqueue one event for every 32-bit word. A route may be represented as a pipelined reservation interval while preserving the correct total number of words and link occupancy.

### C. Runtime-matching task algorithm

For each task:

1. Count required local and route-delivered inputs.
2. Deliver boot/model inputs at their configured time.
3. When all inputs are ready, insert the task into its core's ready queue.
4. If the core is idle, choose the ready task using runtime ordering.
5. Set:

   ```text
   task.start = max(task.ready, core.available)
   task.finish = task.start + task.cost
   core.available = task.finish
   ```

6. At task completion, make local outputs ready and release outgoing routes.
7. If transmit/compute overlap is enabled, allow NIC progress concurrently with later CPU work.
8. Preserve the runtime's output-conflict rule; do not overwrite an output buffer still owned by an in-flight route.

### D. Network algorithm

For each released route:

1. Compute deterministic XY hops.
2. Compute payload and protocol word counts.
3. Wait for the source NIC injection engine.
4. Reserve the first directed link.
5. Propagate a pipelined reservation through subsequent directed links.
6. Attribute wait caused by existing reservations to queue delay.
7. Complete receive DMA or receive-buffer placement.
8. Mark the destination input ready only after receive completion.

The model SHOULD store timestamps for every phase even if the user-facing report aggregates them.

---

## Required elapsed-time decomposition

The safest way to quantify the effect of communication is counterfactual replay.

For the same fixed task graph and placement, compute:

```text
T_full
  Full core, NIC, route, receive, and contention model.

T_no_contention
  Same route sizes, hops, source releases, and transport latency,
  but no delay from sharing links/NIC resources.

T_zero_network
  Route delivery is instantaneous after the producer releases data.

T_zero_runtime_overhead
  Optional: task computation remains, but runtime dispatch and
  bookkeeping overhead are removed.
```

Define:

```text
exposed_contention_ns = T_full - T_no_contention

exposed_transport_ns = T_no_contention - T_zero_network

compute_and_runtime_ns = T_zero_network
```

These terms add to `T_full` in that chosen removal order.

Because component interaction exists, the report MUST state the counterfactual order. A secondary order MAY be reported to show sensitivity. A Shapley-style attribution is possible but unnecessary for the first version.

The compiler SHOULD continue reporting aggregate metrics alongside these values:

```text
sum_task_work_ns
sum_edge_network_service_ns
sum_edge_network_queue_delay_ns
total_payload_words
total_protocol_words
total_word_hops
maximum_directed_link_utilization
maximum_core_utilization
```

Aggregate and exposed metrics answer different questions and must be shown in separate sections.

---

## Digital task-cost implementation options

### Option A: MLIR feature analysis with calibration — recommended

Analyze each final task function and estimate dynamic target instruction classes using static tensor shapes and loop bounds.

Advantages:

- Available before final placement.
- Fast enough for compiler search.
- Generalizes better than one multiplier.
- Preserves interpretability by task family.

Requirements:

- Run after fusion or invalidate and rerun after every structural change.
- Count operations using loop trip counts, not only operation instances in the IR.
- Treat vector operations as vector instructions plus associated loads/stores and loop overhead.
- Count call, descriptor, and route-buffer work.
- Calibrate coefficients using QEMU task traces.
- Validate on models and token sizes not used for calibration.

### Option B: Versioned task-family calibration table — acceptable interim solution

Fit task cycles using features such as:

```text
task kind
tensor rank and shape
element count
bytes read/written
number of route inputs/outputs
number of analog arrays
analog operation counts
number of fused sub-operations
```

Example:

```text
predicted_cycles =
    task_kind_fixed_cost
  + alpha * element_count
  + beta  * local_bytes
  + gamma * loop_iterations
  + delta * route_bookkeeping
```

Advantages:

- Faster to implement.
- Can immediately correct 30x task-family errors.

Limitations:

- Less robust to new lowering patterns.
- Requires model-versioned coefficients.
- Must not extrapolate silently outside its calibrated feature range.

### Option C: Late code-generation feedback — highest fidelity, highest cost

Use a two-stage flow:

1. Create an initial placement.
2. Lower each task or representative task family to RISC-V.
3. Statistically estimate or profile dynamic target instructions.
4. Feed the measured task costs back into placement refinement.

The most expensive variant places SST/QEMU directly in the optimization loop. That is not recommended for general beam expansion, but it is useful as:

- an oracle for a small set of final candidates;
- a calibration generator;
- a paper-validation mechanism.

### Recommendation

Implement Option A with a small Option B fallback table. Use Option C to generate and validate calibration data, not as the default inner-loop scheduler.

---

## Scheduler objective changes

The scheduler's primary objective SHOULD become:

```text
predicted full-deployment warm makespan
```

Recommended secondary objectives, in order:

1. Exposed contention delay.
2. Exposed transport delay.
3. Maximum core utilization/load.
4. Maximum directed-link utilization.
5. Total word-hops.
6. Spatial shared-link pressure as a final tie-breaker.

A lexicographic objective is preferable to an arbitrary weighted sum for the first implementation because its meaning is easier to validate.

An example final comparison key is:

```text
(
  predicted_makespan_ns,
  exposed_contention_ns,
  maximum_core_busy_ns,
  maximum_link_busy_ns,
  total_word_hops,
  spatial_shared_link_pressure
)
```

The final candidate comparison MUST occur after:

- task fusion;
- reduction balancing;
- full island placement;
- route materialization;
- local task-index assignment, or a faithful provisional equivalent.

If these passes are cyclically dependent, use iterative refinement:

```text
initial schedule
  -> finalize/fuse/partition
  -> exact timing analysis
  -> local placement refinement
  -> re-finalize affected metadata
  -> exact timing analysis
```

Convergence SHOULD be bounded and deterministic.

---

## IR attributes and report schema

The compiler SHOULD introduce explicit, versioned fields.

### Task cost provenance

```text
sculptor.timing.cost_model = "golem-qemu-v1"
sculptor.timing.cost_model_revision = 1
sculptor.timing.issue_width = 2
sculptor.timing.vector_bits = 256
sculptor.timing.clock_hz = 1000000000
sculptor.timing.instruction_estimate = ...
sculptor.timing.predicted_cpu_cycles = ...
sculptor.timing.predicted_intrinsic_ns = ...
```

### Deployment estimates

```text
sculptor.timing.predicted_warm_makespan_ns
sculptor.timing.predicted_cold_makespan_ns
sculptor.timing.boot_and_setup_ns
sculptor.timing.sum_task_work_ns
sculptor.timing.sum_edge_network_service_ns
sculptor.timing.sum_edge_network_queue_delay_ns
sculptor.timing.sum_core_queue_delay_ns
sculptor.timing.exposed_transport_ns
sculptor.timing.exposed_contention_ns
```

### Causal event components

```text
sculptor.timing.critical_task_execution_ns
sculptor.timing.critical_core_queue_ns
sculptor.timing.critical_nic_queue_ns
sculptor.timing.critical_link_service_ns
sculptor.timing.critical_link_queue_ns
sculptor.timing.critical_receive_ns
```

### Network configuration provenance

```text
sculptor.timing.link_word_bits
sculptor.timing.protocol_words_per_route
sculptor.timing.link_words_per_cycle
sculptor.timing.router_hop_cycles
sculptor.timing.nic_injection_words_per_cycle
sculptor.timing.rx_dma_words_per_cycle
sculptor.timing.routing_policy
```

Backward-compatible aliases MAY be emitted for one release, but deprecated ambiguous names MUST produce a warning when requested by reporting tools.

---

## Diagnostics

Timing-validation mode SHOULD diagnose the following:

### Error: stale timing metadata

```text
task @mixed_fused_13 carries timing generation 4 but its body was
rewritten in generation 5; rerun task cost analysis
```

### Error or warning: nontrivial zero-cost task

```text
task @token_extract contains loops and memory operations but has no
digital cost; using conservative fallback estimate
```

### Error: incomplete deployment score

```text
timing score excludes 7 digital/reduction islands that were placed
after analog search; full-deployment makespan is unavailable
```

### Error: configuration mismatch

```text
compiler timing assumes issue_width=2 and clock_hz=1 GHz, but the
deployment requests issue_width=4 and clock_hz=3 GHz
```

### Warning: unsupported extrapolation

```text
task feature vector is outside the calibrated range for cost model
golem-qemu-v1; predicted cycles are low-confidence
```

### Report-level error

Any tool attempting to display `sumEdgeNetworkQueueDelayNs / makespan` as "percent of runtime spent in contention" SHOULD fail or label the result explicitly as a non-time-normalized aggregate pressure ratio.

---

## Concrete source areas to change

The exact internal organization may evolve, but the following source areas currently own the affected semantics.

### `TaskLatencyModel.cpp` and its header

Required:

- Replace the universal vector-throughput division.
- Add dynamic instruction-class or calibrated task-cost estimation.
- Add fixed runtime overhead.
- Detect zero-cost executable tasks.
- Version task-cost provenance.
- Re-cost final fused bodies.

### `TaskGraphRoutineFuser.cpp`

Required:

- Preserve semantic workload facts.
- Invalidate derived task timing.
- Stop summing old intrinsic timing as the authoritative fused cost.
- Mark the fused task for mandatory reanalysis.

### `TaskGraphNetworkTiming.cpp`

Required:

- Add per-core execution serialization.
- Add runtime-ready queues and ordering.
- Add source-NIC injection serialization.
- Retain directed-link reservation.
- Add protocol words and receive completion.
- Record causal event parents.
- Add counterfactual timing scenarios.

### `include/sculptor-mlir/Dialect/Sculptor/Transforms/task_timing/TaskGraphTimingAnalysis.h`

Required:

- Separate aggregate, exposed, and causal metrics.
- Add task/core/NIC/link/RX resource-state structures.
- Add timing-model configuration and provenance.

### `TaskGraphTimingProfileBuilder.cpp`

Required:

- Build the critical path from recorded event parents.
- Do not sum every zero-slack branch as one elapsed chain.
- Report core-queue and network-queue causes separately.

### `TaskGraphTimingIRCodec.cpp` and `include/sculptor-mlir/Dialect/Sculptor/Transforms/TaskGraphTimingAttrs.h`

Required:

- Encode the new unambiguous metric names.
- Preserve compatibility only through documented deprecated aliases.
- Encode model revision and hardware parameters.

### `task_schedulers/greedy/GreedyTimingSearch.cpp`

Required:

- Rename edge-sum values as proxies.
- Include complete placements.
- Run exact event replay on completed beam candidates.
- Use predicted makespan as the final primary objective.

### `task_schedulers/greedy/GreedyHeuristic.cpp`

Required:

- Rename link pressure to spatial shared-link pressure.
- Stop assigning it time units.
- Keep it only as a search feature or tie-breaker unless temporal overlap is modeled.

### `task_schedulers/TaskGraphScheduleReport.cpp`

Required:

- Separate aggregate work from makespan decomposition.
- Print warm/cold timing boundaries.
- Print model and hardware provenance.
- Refuse misleading runtime percentages.

### `lib/Dialect/Sculptor/Transforms/ExportTaskGraphSimModel.cpp`

Required:

- Export all parameters needed to reproduce compiler timing.
- Export task instruction/cost features.
- Export route payload and protocol counts.
- Export local task ordering and selected runtime policy.

### `docs/pages/placement.md`

Required:

- Document the corrected resource model.
- Document remaining approximations.
- Define every timing and heuristic metric.
- State that spatial link pressure is not contention.
- State whether reported timing is warm or cold.

---

## Recommended implementation sequence

### Phase 0: Stop reporting invalid conclusions

1. Rename aggregate network metrics.
2. Remove elapsed-time percentages derived from aggregate edge sums.
3. Rename link-pressure and greedy communication terms as proxies.
4. Add timing-model provenance to reports.

This phase prevents new experimental conclusions from being based on invalid labels. It does not yet make the scheduler accurate.

### Phase 1: Match runtime resource ordering

1. Add one CPU execution resource per tile.
2. Match runtime ready-task ordering.
3. Add source-NIC serialization.
4. Add protocol words.
5. Add receive-DMA completion.
6. Emit an event trace with causal parents.

This phase corrects the missing same-core and injection queues.

### Phase 2: Replace the digital task cost

1. Create task-cost feature analysis.
2. Add fixed runtime overhead.
3. Add fallback task-family calibration.
4. Invalidate timing after fusion.
5. Re-cost all final tasks.
6. Diagnose executable zero-cost tasks.

This phase addresses the largest observed error.

### Phase 3: Add exposed-time attribution

1. Implement full replay.
2. Implement no-contention replay.
3. Implement zero-network replay.
4. Generate causal critical-chain attribution.
5. Report aggregate and exposed metrics separately.

### Phase 4: Integrate the corrected model into scheduling

1. Include all analog, digital, and reduction islands.
2. Retain cheap proxies during beam expansion.
3. Replay complete beam candidates.
4. Select by predicted makespan.
5. Add bounded local refinement if placement completion occurs in later passes.

### Phase 5: Increase hardware fidelity only where declared

Potential later additions:

- timed L1/memory-hierarchy backend;
- local-memory bandwidth and bank contention;
- finite router buffers and credit backpressure;
- analog I/O resource conflicts;
- detailed issue/execute pipeline resources;
- branch and cache behavior.

These features SHOULD remain parameterized. They must not be implied when the native untimed-memory backend is selected.

---

## Acceptance tests

### Unit tests for task resources

#### One core, two independent tasks

Both tasks are ready at time zero and assigned to one core.

Expected:

```text
makespan = task_A_time + task_B_time
```

They MUST not overlap.

#### Two cores, two independent tasks

One task is assigned to each core.

Expected:

```text
makespan = max(task_A_time, task_B_time)
```

#### Runtime-index ordering

Multiple tasks become ready simultaneously on one core.

Expected:

- The chosen order matches `runtime.task_index`.
- The event trace records core-queue delay for later tasks.

#### Output-buffer conflict

A task would overwrite an output whose route is still in flight.

Expected:

- Runtime policy behavior matches `deployment_runtime.cpp`.
- The reason for the delay is recorded explicitly.

### Unit tests for network resources

#### Same directed link, simultaneous release

Two equal routes become ready together and use the same directed link.

Expected:

- One route waits.
- Full makespan exceeds no-contention makespan by the known reservation delay.

#### Same directed link, nonoverlapping release

The second route becomes ready after the first has cleared the shared link.

Expected:

- Spatial shared-link pressure is nonzero.
- Temporal contention delay is zero.

This test is essential because it proves the difference between the two concepts.

#### Opposite directions

Two routes traverse the same physical neighbor connection in opposite directions.

Expected:

- They operate independently if the platform has separate directional channels.

#### Disjoint paths

Two simultaneous routes share no directed links.

Expected:

- No link queueing.

#### Source NIC serialization

Two routes originate on one tile but use different first directions.

Expected:

- Injection is serialized according to the one-NIC contract.
- Later links may proceed independently once injected.

#### Protocol overhead

A one-word payload traverses one hop.

Expected:

- Service accounts for one payload word plus the configured protocol words.

#### Receive DMA

Two messages arrive at one tile while the receive engine is occupied.

Expected:

- Receive completion is serialized if the hardware contract specifies one RX engine.
- Consumer readiness follows receive completion, not final-link arrival.

### Unit tests for causal attribution

#### Earlier producer, later arrival

Producer A finishes after producer B, but B's route arrives last.

Expected:

- B's route is the readiness parent.

#### Core queue dominates

All inputs arrive, but another task occupies the core.

Expected:

- The previous local task is the causal start parent.
- The delay is core queueing, not communication.

#### Parallel zero-slack branches

Two equal branches join.

Expected:

- No elapsed component is counted twice.
- Ties are represented explicitly.

### Unit tests for task cost

#### Vector arithmetic loop

Expected:

- Vector arithmetic is counted as vector instructions.
- Loop, load, store, and control instructions are also counted.

#### Tensor copy loop

Expected:

- Nonzero cost despite zero arithmetic.

#### Fused task

Expected:

- Cost is generated from the fused function.
- Pre-fusion timing attributes cannot survive as authoritative values.

#### Model revision mismatch

Expected:

- A diagnostic identifies the incompatible compiler and deployment timing configurations.

### Scheduler tests

#### Reduction placement

Two deployments differ only in reduction location.

Expected:

- The full score changes.
- The timing search cannot report the same completed makespan if core and route queues differ.

#### Link pressure without contention

One placement has greater spatial link sharing but no temporal overlap.

Expected:

- The spatial heuristic differs.
- Exposed contention remains zero.

#### Exact final reranking

A proxy-preferred placement loses under full event replay.

Expected:

- The final scheduler selects the candidate with lower predicted makespan.

---

## End-to-end validation

Validation MUST use fixed placements so task-cost and event-model accuracy can be tested independently of search quality.

Recommended workloads:

- Small synthetic single-tile digital kernels.
- Small analog MVM-to-digital chains.
- Two-core producer/consumer examples.
- 2x2 and 3x3 routed task graphs.
- GPT-2 at 4, 8, 16, and 32 tokens.
- Both analog and digital GPT-2 variants.
- At least snake, random, greedy, and greedy-timing schedules.
- Balanced and unbalanced reduction variants.
- A convolution workload once the remaining streaming convolution work is stable.

### Required comparisons

For every deployment, compare:

- compiler warm makespan versus SST warm makespan;
- task-family predicted cycles versus measured task cycles;
- compiler aggregate payload words versus SST logical-route payload words;
- compiler word-hops versus SST word-hops;
- compiler exposed contention versus an SST no-contention counterfactual;
- compiler schedule ranking versus SST schedule ranking.

### Initial acceptance thresholds

Suggested initial thresholds:

| Metric | Initial target |
|---|---:|
| Held-out makespan mean absolute percentage error | <= 15% |
| Held-out task-family median cycle error | <= 15% |
| Held-out task-family 90th percentile cycle error | <= 25% |
| Scheduler rank correlation | Spearman >= 0.80 |
| Best-candidate recall | SST best is in compiler top 3 |
| Aggregate route payload mismatch | 0% unless documented protocol transformation exists |

For small exposed components, percentage error is unstable. Use:

```text
allowed_component_error =
    max(20% of measured component, 1% of full makespan)
```

Calibration and validation datasets MUST be separated. For example:

- Calibrate on GPT-2 token counts 4 and 16 plus synthetic kernels.
- Validate on token counts 8 and 32 plus a separate model.

The final paper evaluation MUST include held-out workloads.

---

## Monotonic sanity tests

Even before absolute accuracy is achieved, the model MUST satisfy:

- Increasing clock frequency decreases CPU time proportionally where CPU-bound.
- Increasing issue width never increases predicted CPU performance time.
- Increasing MVM latency never decreases makespan.
- Reducing link words per cycle never decreases transport time.
- Increasing route payload never decreases route service time.
- Adding a hop never decreases ideal route latency.
- Removing contention cannot increase makespan.
- Removing the network cannot increase makespan.
- Moving independent tasks to separate idle cores cannot make them serialize.
- Placing two tasks on one core cannot make them execute simultaneously.

Failures in these tests indicate semantic defects rather than calibration error.

---

## Alternatives and tradeoffs

### Alternative 1: Minimal safe reporting

Stop producing predicted runtime percentages. Report only:

- task counts;
- byte/word volume;
- word-hops;
- maximum per-core work;
- maximum per-link spatial pressure;
- placement proxy score.

Advantages:

- Very fast to implement.
- Prevents false conclusions.

Disadvantages:

- Does not predict runtime.
- Cannot reliably compare compute/communication tradeoffs.
- Does not meet the longer-term research need.

This is an acceptable emergency fallback while the corrected model is under development.

### Alternative 2: Calibrated task costs plus resource event simulation

Use task-family or MLIR-derived costs with per-core, NIC, link, and RX resources.

Advantages:

- Strong balance of accuracy, speed, and explainability.
- Suitable for beam-candidate reranking.
- Directly supports paper-quality breakdowns after validation.

Disadvantages:

- Requires careful calibration and versioning.
- Still abstracts microarchitectural details.

This is the recommended implementation.

### Alternative 3: SST/QEMU in the compiler optimization loop

Compile and simulate multiple placement candidates directly.

Advantages:

- Highest agreement with the declared simulator.
- Useful as a final oracle.

Disadvantages:

- Too slow for broad search.
- Complicates deterministic compiler testing.
- Risks tuning to simulator idiosyncrasies rather than architecture.

Use this for calibration and final candidate selection only.

### Alternative 4: Static post-codegen instruction analysis

Lower tasks to RISC-V and estimate dynamic counts from disassembly plus static loop bounds.

Advantages:

- Closer to the executable than MLIR operation counting.
- Avoids full QEMU execution for every candidate.

Disadvantages:

- Harder to recover dynamic control behavior.
- Placement may be needed before final code generation.
- Requires a feedback/refinement flow.

This is a strong longer-term direction.

---

## Approaches that should not be used

### Do not multiply all digital costs by 14

The 14x full-makespan error is a symptom, not a universal coefficient. Individual task families have different errors, and a single multiplier will not preserve schedule ranking.

### Do not reduce the contention weight until the percentages look plausible

The problem is semantic: aggregate queue delay is being interpreted as exposed elapsed time. A new weight cannot fix double counting or hidden parallel delay.

### Do not widen the link to calibrate the model

The architecture specifies 32-bit tile-to-tile transfers. Link width is a real architectural parameter, not an error-correction knob.

### Do not subtract aggregate contention from aggregate network latency

That still produces aggregate edge service, not critical exposed time.

### Do not calibrate only on the current GPT-2 trace

The model must generalize across token counts, analog/digital mappings, schedules, reductions, and at least one held-out workload family.

### Do not compare compiler warm timing with SST cold timing

Both boundaries must be explicit.

### Do not use host wall-clock time

This work concerns simulated target cycles and nanoseconds.

---

## Required deliverables from the compiler change

The compiler engineer should provide:

1. A written metric contract matching the terminology in this document.
2. A versioned `TaskCost` model.
3. Post-fusion cost invalidation and regeneration.
4. A deterministic per-core/NIC/link/RX event simulator.
5. Full, no-contention, and zero-network replay modes.
6. Causal critical-chain export.
7. Complete-placement scoring including digital and reduction islands.
8. Exact final reranking for completed scheduler candidates.
9. New IR/report fields with hardware and model provenance.
10. Unit tests for all resource and attribution semantics.
11. End-to-end compiler-versus-SST validation.
12. Documentation of remaining abstractions and unsupported cases.

The result SHOULD export machine-readable JSON or CSV containing:

```text
configuration
task cost model version
compiler revision
graph identifier
placement identifier
warm/cold boundary
predicted makespan
aggregate task work
aggregate route service
aggregate route queue delay
exposed transport
exposed contention
per-core utilization
per-link utilization
causal critical-chain events
per-task predicted cycles
per-route timing phases
```

This data is necessary for validating the compiler and for producing defensible research figures.

---

## Definition of done

This work is complete when:

1. No compiler report describes aggregate edge latency or link-pressure proxy values as elapsed runtime shares.
2. Nontrivial digital and data-movement tasks receive nonzero, versioned costs.
3. Fused tasks are re-costed from their final implementation.
4. Tasks assigned to one core serialize according to the runtime policy.
5. Routes serialize through the source NIC and shared directed links.
6. Consumer readiness includes route arrival and receive completion.
7. The compiler produces a causal critical chain.
8. Exposed transport and contention are computed through documented counterfactual replay.
9. Reduction and digital-only placement affect the final makespan score.
10. Final beam candidates are ranked using the complete timing model.
11. The model meets the initial held-out error and ranking targets or reports where and why it does not.
12. Every estimate records the clock, issue width, vector width, MVM latency, link parameters, runtime policy, memory backend, compiler revision, and timing-model revision.

---

## Final recommendation

The immediate priority is not to make communication look less expensive. It is to make every reported number mean exactly what its label claims.

The recommended path is:

```text
correct metric names
  -> serialize per-core and NIC resources
  -> replace high-level digital-op throughput with calibrated instruction cost
  -> re-cost fused tasks
  -> compute exposed delay using counterfactual replay
  -> score complete placements
  -> validate ranking and absolute timing against held-out SST runs
```

The expected outcome is a compiler that can answer three separate questions correctly:

1. **How much work exists?**
2. **Where are the architectural resources pressured?**
3. **What actually determines end-to-end execution time?**

Those answers are all valuable, but they are not interchangeable. Keeping them separate is the key change required for a trustworthy compiler scoring model and a defensible analog-versus-digital architecture study.
