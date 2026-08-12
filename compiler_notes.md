# Compiler Notes: GPT-2 Critical-Path Problem

## Purpose

This document describes why the current GPT-2 compiler output does not gain much performance from shorter mesh routes.

The problem is not the time that Sculptor needs to compile the model. The generated task graph and placement control the simulated runtime.

This document uses results from the 12-decoder GPT-2 model. The model uses 16 tokens and eight parallel digital workers.

## Executive summary

The compiler reduces total word-hops, but it does not reduce the task chain that controls model completion.

The current task graph uses full-tensor producer barriers and large fan-in operations. A consumer waits until all required producers and transfers are complete.

Most critical waits occur before the source creates a packet. The packet then crosses the mesh in microseconds.

The compiler also uses spatial communication estimates as placement scores. These estimates do not predict the measured completion time.

The principal mitigation has two parts:

1. Use a time-aware scheduler that minimizes predicted completion time.
2. Replace full-tensor barriers with shard-level data flow and local reduction trees.

## Existing problem

### The placement result looks better than the runtime result

The following results use the same Merlin router model and the same workload:

| Metric | Snake | Greedy L3 | Change |
|---|---:|---:|---:|
| Simulated runtime | 87.162496 ms | 86.871505 ms | -0.33% |
| Injected words | 12,251,955 | 12,251,955 | 0% |
| Directional word-hops | 204,412,542 | 55,126,284 | -73.03% |
| Aggregate packet-transit time | 827.959 ms | 1,940.428 ms | +134.36% |
| Aggregate endpoint-queue time | 3,942.929 ms | 3,503.639 ms | -11.14% |

Greedy L3 removes 73% of the word-hops. However, the simulated runtime decreases by only 0.33%.

The word-hop reduction decreases total link work. It does not remove payload bytes or long producer computations.

The aggregate packet-transit value is not an elapsed-time value. Many packets move at the same time on different links.

### The new router produces the same conclusion

The Mittens router changes the Greedy L3 runtime from 86.8715 ms to 86.6628 ms.

Mittens reduces aggregate packet-transit time by 42.7%. The complete model gains only 0.24%.

This result shows that the network model can move packets faster. The final output still waits for late producers.

The Merlin and Mittens configurations use different link delays. Therefore, this comparison is not a router-only performance comparison.

### The critical waits occur before packet creation

The traced Merlin run shows two large waits on tile 0.

```text
Start
  |
  |  Upstream task graph: approximately 52.57 ms
  v
Critical transfer: approximately 3 us
  |
Task 4032: 6.746 us
  |
  |  Upstream task graph: approximately 34.17 ms
  v
Critical final transfer: approximately 14 us
  |
Task 6560: 89.474 us
  |
Done: 86.87 ms
```

Task 4032 starts at 52.584278 ms. Its last required input becomes ready at 52.582754 ms.

The corresponding route needs 3.326 us from injection start to destination readiness. The producer creates that route at 52.579428 ms.

Task 6560 starts at 86.780518 ms. Its last required input becomes ready at 86.777955 ms.

The last-arriving route needs 14.017 us from injection start to destination readiness. Its source starts injection at 86.763938 ms.

Task 4032 finishes at 52.591024 ms. The next critical source does not start its final injection until 86.763938 ms.

This 34.173 ms interval is upstream work and dependency delay. It is not packet transport delay.

## Why the compiler is the bottleneck

### Meaning of compiler bottleneck

The compiler executable is not the runtime bottleneck. The compiler creates the execution structure that causes the runtime bottleneck.

The compiler controls these properties:

- Task boundaries.
- Producer and consumer dependencies.
- Digital work distribution.
- Reduction and assembly structure.
- Task placement on tiles.
- Route endpoints.
- The number of synchronization barriers.

The router cannot move data before a producer creates that data. A faster router cannot correct a late producer or a full-tensor barrier.

### The scheduler minimizes the wrong quantity

The current placement score gives large value to spatial quantities. These quantities include hop count, link pressure, and aggregate communication cost.

These values help the compiler compare placements. However, they do not directly predict the final completion time.

A valid completion-time model must include temporal state:

- The time when each task becomes ready.
- The time when each tile becomes free.
- The measured execution cost of each task.
- The time when each NIC becomes free.
- The serialization time for each packet.
- The reservations on each directed link.
- The destination receive and DMA time.

The scheduler can reduce noncritical routes while it leaves the final producer chain unchanged. This placement has a better score but similar runtime.

### The digital cost model is too optimistic

The compiler cost model underestimates important digital and mixed tasks. It also misses some runtime and data-movement work.

As a result, the scheduler treats expensive producers as inexpensive tasks. The scheduler then gives too much importance to route distance.

This error hides load imbalance. It also hides long serialized work on tiles that produce critical outputs.

### The task graph uses bulk barriers

The current execution is similar to this structure:

```text
Worker 0 -- compute complete shard --+
Worker 1 -- compute complete shard --+
Worker 2 -- compute complete shard --+--> wait for all --> assemble --> next task
Worker 3 -- compute complete shard --+
```

The consumer does not start when an early shard becomes available. It waits for the complete set of required inputs.

The slowest producer controls each fan-in operation. A shorter route for an earlier producer does not change the fan-in completion time.

The workers also send results at similar times. This behavior creates short traffic bursts and many-to-one contention.

### Parallel distribution adds work

The digital-worker pass does more than divide the original computation. It can add partition, shard, route, assembly, and reduction tasks.

More workers decrease the work in each shard. They also increase control work, synchronization, routes, and fan-in pressure.

The compiler does not currently calculate this break-even point with a measured temporal model.

### Shorter wormhole routes have a limited latency benefit

A wormhole packet has three principal latency terms:

```text
packet latency = payload serialization + head pipeline + exposed queue delay
```

A 768-word payload needs at least 768 link cycles on a 32-bit link. A shorter route does not remove this serialization term.

The shorter route decreases the head-pipeline delay and total link work. The decrease can be small relative to task execution time.

## What is going on during execution

The following sequence occurs at a typical distributed layer:

1. The runtime waits for the inputs of each producer task.
2. Each tile executes its complete producer or shard task.
3. The runtime creates a route after the complete output is available.
4. The NIC serializes the output into 32-bit words.
5. The mesh moves the packet to the consumer tile.
6. The destination completes its receive DMA operation.
7. The consumer waits for all other required inputs.
8. The consumer starts after the last input becomes ready.

The network only controls steps 4 through 6. The compiler controls the complete sequence and its dependency boundaries.

The current traces contain 776,991 endpoint packets and 12,251,955 injected words. This traffic can create large aggregate network totals.

Most network activity overlaps other network activity or computation. Only exposed delay on the causal critical path increases the final runtime.

## Potential solution

### 1. Build a temporal scheduler

The compiler must estimate the earliest start and finish time for every task.

Use this recurrence as the base model:

```text
task_ready = maximum arrival time of all required inputs
task_start = max(task_ready, core_available[tile])
task_finish = task_start + calibrated_task_cost
```

Calculate each route with explicit resource state:

```text
route_start = max(producer_finish, nic_available[source])
route_finish = route_start
             + packet_serialization
             + router_pipeline
             + reserved_link_delay
             + receive_dma
```

Update `core_available`, `nic_available`, and each directed-link reservation after each scheduled event.

The primary objective must be final output completion time. Word-hops and energy estimates can remain secondary objectives.

### 2. Calibrate all task costs

Use QEMU and SST measurements to calibrate digital task costs. Keep separate costs for computation, memory, runtime, partition, reduction, and assembly.

Each nontrivial task must have a nonzero cost. The compiler must not treat descriptor work or tensor movement as free.

The cost model must use the declared issue width, vector width, memory backend, and clock frequency.

### 3. Add shard-level data flow

The compiler must preserve shard identities after digital work distribution. Each shard must have independent readiness and route information.

A producer must send a completed shard without waiting for unrelated shards. A consumer must start when its required shard set is ready.

This change creates overlap between producer computation, network transfer, and consumer computation.

```text
Worker 0 -- shard 0 --> local consumer 0 --+
Worker 1 -- shard 1 --> local consumer 1 --+--> reduction tree --> next task
Worker 2 -- shard 2 --> local consumer 2 --+
Worker 3 -- shard 3 --> local consumer 3 --+
```

The compiler must use tensor views for partitioned inputs when the memory contract permits them. It must not copy a full partition unnecessarily.

### 4. Replace central assembly with a reduction tree

Place each first-stage reduction near its producer pair. Continue the reduction through local tree levels.

This structure removes a large many-to-one burst. It also decreases the distance of critical reduction edges.

The compiler must balance the tree with measured producer finish times. A geometrically balanced tree can remain temporally unbalanced.

### 5. Select the worker count with a break-even model

For each candidate worker count, calculate these terms:

```text
benefit = serial_compute_time - parallel_compute_time

cost = partition_time
     + additional_route_time
     + reduction_time
     + assembly_time
     + exposed_contention_time
```

Use more workers only when `benefit > cost`. Record the selected worker count and the calculated terms in the compiler report.

### 6. Export a causal critical chain

Each route trace must retain its source task, destination task, and causal predecessor. Each task must retain its global task identifier.

The compiler report must separate these quantities:

- Aggregate task work.
- Aggregate network service.
- Aggregate network queue delay.
- Exposed transport delay.
- Exposed contention delay.
- Final makespan.

Do not report aggregate network values as percentages of elapsed runtime.

## Required validation

Use one compiled deployment with three network modes:

| Mode | Transport | Contention | Purpose |
|---|---|---|---|
| Ideal | Zero cost | None | Measure the computation and dependency floor |
| Finite | Real width and distance | None | Measure exposed transport cost |
| Full Mittens | Real width and distance | Modeled | Measure exposed contention cost |

Calculate the contributions as follows:

```text
exposed_transport = finite_makespan - ideal_makespan
exposed_contention = full_makespan - finite_makespan
```

Then run worker counts 1, 2, 4, 8, 16, and 32. Use the same model, precision, memory backend, and mesh for all runs.

Record these values for each run:

- Final makespan.
- Critical-chain task time.
- Critical transport time.
- Critical contention time.
- Partition and assembly time.
- Maximum tile load.
- Injected words.
- Directional word-hops.
- Maximum directed-link occupancy.
- Final producer finish time.

## Acceptance criteria

The mitigation is complete when all of these statements are true:

1. The compiler predicts makespan within a documented error limit for the calibration workloads.
2. The compiler identifies the same final critical producer chain as the simulator trace.
3. The compiler does not classify aggregate communication as elapsed-time contribution.
4. Each distributed shard can become ready and move independently.
5. A reduction tree replaces the central fan-in for supported distributed operations.
6. The worker sweep shows the expected compute-to-communication tradeoff.
7. Shorter critical routes reduce exposed transport time in the finite-network mode.
8. Additional workers increase exposed contention after the network saturation point.
9. Every configuration produces numerically correct model output.

## Recommended implementation order

1. Calibrate the digital, memory, partition, reduction, and assembly costs.
2. Add the temporal task and resource scheduler.
3. Export complete causal identifiers for tasks and routes.
4. Add the three network validation modes.
5. Add shard-level readiness and routes.
6. Add local reduction-tree placement.
7. Add automatic worker-count selection.
8. Run the complete worker and placement sweeps.

## Final conclusion

The current compiler successfully reduces spatial communication work. It does not reduce the late producer chain that controls GPT-2 completion.

The task graph delays packet creation and requires full-tensor fan-in. Therefore, faster routes produce only small end-to-end gains.

A time-aware scheduler will identify the real critical chain. Shard-level data flow will expose useful overlap and a measurable parallelism tradeoff.
