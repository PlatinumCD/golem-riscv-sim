# Behavioral contracts during controller extraction

The reference remains `src/`. R1–R5 comparison evidence is in `TASK_QUEUE.md`.
The following contracts were identified before moving the RX/TX state machines.

## Shared resources and event order

- There is one SPM timing model per tile. CPU, TX lanes, RX and global DMA use
  its existing banks and ports; controller extraction must not duplicate it.
- `serviceBridge` services RX before TX, then analog. Reservations in the common
  arbiter depend on this order; changing it would be a model change.
- RX progress can wake a guest blocked on TX. Keep the receive-ready and
  transmit-capacity wake checks in their existing order.
- FIFO accounting must retain `offset <= available <= scheduled <= word_count`,
  `scheduled - available == in_flight`, and the resident-plus-in-flight bound.
  One DMA beat is in flight per lane; setup is charged once per burst.
- QEMU's copied payload remains the functional snapshot. Timed DMA controls
  availability; this refactor does not transfer functional SPM ownership.
- Packet metadata/frame state, packet IDs and successful-send counters commit
  only after NIC acceptance. `spaceToSend` can succeed for a different lane
  while the selected lane's `send` still rejects.
- T1 and T2/T4 currently use different scheduling paths. Preserve ordering and
  destination/frame lane affinity before considering consolidation.

## Existing measurement/lifecycle limitations to keep explicit

The TX audit found these in the reference implementation, before extraction:

- Multi-lane scalar sends omit some profiling emitted by the T1 path.
- Progress snapshots omit some multi-lane pending descriptor state.
- `outgoingPacketsIdle` is a local pending-work query, not a proof that bridge
  queues, open frames and every packet in the network are drained.
- Opportunity intervals are observed at specific call sites, not every state
  change. Moving those observations changes interval statistics.
- `networkWordHops` and `networkEndpointQueueTicks` have TX-only writers;
  `networkTransitTicks` is RX-owned. They are not additive elapsed intervals.

These must not be silently corrected during extraction and then called timing
equivalent. Measurement fixes belong to R10 with versioned semantics and explicit
before/after evidence; any lifecycle behavior fix needs its own regression.

## Separate timing correction validated (R5a)

The original global-DMA submit path adds a CPU-cycle local SPM service duration
directly to an SST-tick submission timestamp. Its single/batch wait then passes
an SST-tick difference to the CPU-cycle scheduler. These are not interchangeable
units. A local-SPM-dominated transfer is required to expose this; a slower global
RAM transfer can hide the error. Reproduction and correction are tracked apart
from RX/TX structural equivalence. R5a now constructs deadlines with the CPU
clock factor and rounds remaining tick intervals up to CPU cycles. An unrelated
completion wake can no longer bypass an outstanding local deadline merely
because a delay is scheduled. All 13 corrected src2 oracle cases pass across
500 MHz, 1 GHz and 2 GHz; the original reference still fails those cases.

## Separate CPU/SPM deadline correction validated (R5b)

Captured events and SPM service carry absolute eligibility deadlines. A buffered
ordinary-memory response may retry pending work but cannot retire future CPU
instructions or unfinished SPM service. A scheduled CPU wake carries a revocable
generation; an obsolete wake cannot advance a newer event. Device completions
must identify the pending step they belong to. These rules remain enforced
across owner extraction; the cacheless oracle checks both buffered and blocking
stores at three CPU clocks. See `sst/execution/R5b.md` for preserved failures and
the intentional timing differences in existing DMA tests.

## Reporting is observational (R10)

Close the existing wait/TX observation intervals at their existing sites, then
copy resource values once. Named summary rendering and task tracing cannot
schedule requests or mutate resource queues. CSV order/values and ordered task,
wait and device traces remain comparison gates. JSON supplies interpretation,
explicit availability and provenance, not new physical measurements.
