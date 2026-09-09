# Measurement semantics

This document defines counter units, availability, and aggregation for the
current simulator. JSON records carry availability metadata; CSV output cannot
distinguish an unavailable value from zero. Use JSON for analysis that needs
to make that distinction.

## Three quantities that must not be confused

- **Elapsed interval:** end timestamp minus start timestamp on one clock.
- **Service sum:** accumulated durations of operations; concurrent operations
  overlap, and their sum can exceed elapsed time.
- **Observed-state duration:** time assigned to the state at the previous
  observation point. Correctness depends on when those observations occur.

SST ticks, CPU cycles, RX-engine cycles, analog cycles, NIC cycles and router
cycles are distinct units. Tick conversion requires the run's SST timebase.
Missing/disabled resources must be marked unavailable, not silently interpreted
as measured zero. TX words include protocol words, not just useful payload.

`cpu_cycles` in both the legacy summary and task trace is accumulated **guest
instruction-issue cost**. Subtracting task-trace `cpu_cycles` gives region guest
issue cycles, not elapsed execution including device waits. Elapsed region time
comes from task-trace `sim_time_ticks` differences and the configured timebase.
Some existing study scripts label the former `measured_cycles`; that label must
not be used to infer end-to-end latency. Historical result schemas need explicit
migration, not a silent reinterpretation of saved numbers.

## Scratchpad

Owner: `ScratchpadTimingModel`. All cycle quantities use the CPU domain. Counts
are charged on **scheduling**, not completion. CPU, global DMA, RX-SPM and TX
lanes share the same bank/port statistics.

| Legacy field | Exact meaning |
|---|---|
| `cpuRequests` | CPU accesses; compact runs add their original access count |
| `dmaTransfers` | Calls to `scheduleDMA`, not uniform logical transfers: TX calls per FIFO-fill beat, RX per burst, global DMA per transfer |
| `dmaBytes` | Requested DMA bytes across all DMA clients |
| `readRequests`, `writeRequests` | Scheduled bank-port beats |
| `bankConflicts` and directional variants | Delayed beats, **not delay cycles**: a delayed beat increments once regardless of delay length |
| `activeCycles`, directional service cycles | Sum of completion minus service start; includes setup, beat issue/latency and bank delay; excludes preceding DMA-frontend queueing |
| `queueCycles` | DMA sum of service start minus caller-supplied current cycle; excludes queueing before the call and excludes bank delay already counted in service |
| `max*ServicedSameCycle` | Maximum same-cycle scheduled beat issues across banks, not average bandwidth |

For successful schedules:

```
activeCycles = readServiceCycles + writeServiceCycles
bankConflicts = readBankConflicts + writeBankConflicts
DMA completion - caller current = frontend queue + service
```

Existing conflict totals cannot reconstruct bank-delay cycles. Existing service
totals cannot reconstruct a busy-time union or separate setup from bank delay.
RX advances its supplied current cycle to preserve frontend order, so some RX
queueing precedes the SPM call and is absent from `queueCycles`.

## TX and RX

| Counter family | Scope and aggregation |
|---|---|
| TX packets/words | Successful NIC acceptance; includes raw/protocol/header words |
| RX packets/words | RX network-completion processing, not guest consumption |
| Summary `network_packets/words` | TX only; progress network totals instead combine TX and RX |
| `networkWordHops` | TX words times geometric mesh hops; not measured router activity |
| `networkEndpointQueueTicks` | Sum per accepted packet from controller-ready to acceptance; excludes earlier bridge residence, can include source DMA waits, and repeats burst age across fragments |
| `networkTransitTicks` | RX sum per packet from TX acceptance to RX completion; includes NIC queueing, transport, endpoint admission/dequeue and modeled serialization; **not pure wire latency** |
| RX DMA transfers/words/service | Charged at authorization after timing/invalidation; CPU-cycle service excludes invalidation/guest-ack delay |
| TX DMA service | CPU-cycle service charged when SPM beats are scheduled; includes setup/bank delay; excludes frontend queueing |
| T1 blocked ticks/events/retries | `spaceToSend` failure episodes; first failure counts as a retry; does not fully cover rejection by the following `send` |

Multi-lane scalar sends currently omit some inject profiling and endpoint queue
accounting. Legacy TX blocked episodes do not fully measure T2/T4 backpressure.
RX trace has schedule and complete records: sum one phase, not both.

### TX opportunity observations

The observer runs at service entry and final reporting. Durations belong to the
previously observed state. A lane is "active" when it holds a burst, not only
when it transmits. Ready counts include assigned bursts even with empty FIFOs.

- Ready/active/direction buckets: CPU cycles within the observation window.
- Serialization stalls: observed excess ready demand over lane count; a demand
  proxy, not proven lost throughput. Independent stalls compare directions to lanes.
- Delayed bytes: positive observed queue-byte increases under excess demand;
  not unique delayed payload. Byte-cycles are a weighted duration integral.
- Queue occupancy: bridge burst descriptors, excluding assigned lanes/scalar queue.
- FIFO empty/full: any-lane predicate cycles; lane variants sum lane-cycles.
  The full predicate uses resident words, not resident plus in-flight words.

Each complete histogram sums to `transmitOpportunityObservedCycles`; the printed
ready histogram omits bucket zero. Predicates overlap and cannot be added as
independent elapsed-time components. Legacy TX progress omits multi-lane bursts.

## Wormhole NIC and router

These statistics require instantiated components and enabled SST statistics.

| Statistic | Unit and boundary |
|---|---|
| NIC injected/received flits | 32-bit flits sent to/accepted from router |
| NIC completed packets | Tail arrivals queued for endpoint delivery |
| NIC packet latency | SST ticks, actual head injection to destination tail arrival |
| NIC injection occupancy | Queued flits across lanes, sampled before sends; Sum is sampled flit-cycles |
| NIC credit stalls | NIC cycles with nonempty aggregate queue and no credit in any lane; an empty credited lane can mask a blocked occupied lane |
| Router forwarded flits/packets | Per-output flits/tails; sums across outputs count traversals |
| Router input occupancy/full | Per-input pre-send flit samples / cycles at capacity |
| Router arbitration stalls | Blocked ready-head times router-cycles; can exceed one count per output per cycle |
| Router credit stalls/link busy | Per-output router cycles with selected zero-credit demand / at least one transmitted flit |
| Cardinal outputs active | Concurrent active cardinal-link count, including transit traffic |

Idle clocks stop. Occupancy and credit-stall intervals during credit sleep are
backfilled on wake, but cardinal-active zero samples are not. Terminal sleeping
intervals lack final backfill. Sample counts therefore have different denominators;
do not equate their means with whole-run link utilization. Experimental router
burst forwarding charges future serialization/busy counts up front.

## Conditional end-to-end conservation

Only for genuinely drained, endpoint-complete, matching 32-bit Tile traffic:

```
sum TX words = NIC injected flits = NIC received flits = sum RX words
sum TX packets = NIC completed packets = sum RX packets
cardinal-router flits = TX word-hops   (matching minimal mesh routing)
```

All router outputs additionally include destination-local delivery. RX DMA words
exclude headers/software-consumed traffic. SPM service totals already contain TX
and RX-SPM service; adding them double counts. Local controller idle predicates
do not prove global drain. Pending RX counts mix packets/bursts/frames, and
global-DMA pending tokens include completed-but-unretired and teardown entries.

The JSON report exposes these scopes and unavailable values explicitly. Whole-run busy
time, true bank-delay cycles, event-complete TX observation and corrected NIC
stall semantics require additional measurement, not relabeling existing values.

## Output contract and ownership

Physical owners accumulate counters. At the existing final observation site,
Tile closes the current CPU wait and TX observation interval, then copies one
`TileMeasurementSnapshot`. The reporting functions consume that owned value;
they cannot schedule requests, poll queues or change resource accounting.
`TaskTrace` separately owns task attribution and task-trace output. The CPU
controller owns execution; starting or ending a trace does not execute work.

Every enabled tile summary writes both:

- `tile-N-summary.csv`: unchanged legacy metric names, ordering and values.
- `tile-N-summary.json`: schema version 1, with unit, clock domain, aggregation,
  known scope and availability for each metric. JSON is the authoritative
  interpretation; an unavailable legacy CSV zero is not a measured zero.

Availability is `available`, `disabled`, `not_measured` or `unknown`.
Unavailable JSON values are null. In particular, incomplete T2/T4 endpoint
queue/blocked counters are marked `not_measured`, even where the legacy CSV
retains a partial accumulated value. Disabled physical resources are explicit.
All three TX observation histograms include bucket zero and must reconcile to
their shared observation window; they remain observations, not true busy time.

JSON includes the compiled source-tree/input identity, the exact resolved tile
configuration reference, the SST timebase and actual CPU/RX/analog converter
factors. NIC/router/global-RAM factors remain unknown in the tile report because
those clocks belong to external components. Use their own resolved parameters;
never infer them from the tile's mesh scheduling clock. Full binary hashes,
toolchain identity and run commands remain in build/regression manifests.

The CSV and JSON are a compatibility pair, not an atomic transaction. Accept
them only after the enclosing run succeeds and the output validator passes.
Integers retain full uint64 precision; JavaScript consumers need a lossless
integer parser for values beyond their safe integer range.
