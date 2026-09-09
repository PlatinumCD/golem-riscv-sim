# Completed cleanup audit

R0–R11 are complete. Named Tile snapshot/TaskTrace integration and the final
hardware/reporting gates pass; [ACCEPTANCE.md](source-refactor-acceptance.md) records the evidence.
`src/` remains the untouched reference and default build selection; no
compiler/model suite is included.

## Implemented removals and hardening

| Change | Current implementation / preserved boundary |
|---|---|
| Dead NIC whole-packet injection helper | Removed `tryBeginPacketBurst` declaration/definition and exclusively used cached burst state. The pre-removal audit found no caller. The accepted `packet_burst_coalescing` parameter and live router/receive burst paths remain. |
| NIC cached mesh dimensions | Removed initialization-only `meshWidth_`/`meshHeight_` copies; validated configuration and emitted parameters remain. |
| Duplicate GlobalRAM constructor validation | Removed repeated dependency-mode and active-tile checks now owned by immutable central configuration. Enum conversion, active-mask construction and request-time validation remain. |
| R9 extraction adapters | Physical owners consume neutral `cpuActions.h` values directly; reconstructed `QemuSyncEvent` envelopes and single-case execution switches were removed from those adapters. CPU still owns replay; device owners do not borrow its queues. |
| Complete default checks | Replaced the four-default spot check with the full 73-parameter tile golden plus positive resource configuration checks. |
| Mesh overflow validation | Router/NIC products use widened arithmetic; router dimensions outside the endpoint-ID space and inconsistent NIC dimensions are rejected. Added overflow-input checks. This changes invalid-input rejection, not valid-machine timing. |
| Stale SST guide content | Removed the deleted private-L1 fixture requirement, documented cacheless StandardMem, updated initial/runtime/local capture descriptions and repaired relocated documentation links. Deleted L1/L2 tests were not restored. |
| Positional reporting API | Removed the 45-argument overload and migrated callers/tests to named owned snapshots; independent legacy CSV golden and actual paired runs verify compatibility. |
| Task/network adapter leftovers | Removed Tile's reconstructed task/network sync envelopes, redundant switches and trace-state ownership; neutral CPU actions and TaskTrace now own those boundaries. |
| Repeated diagnostic formatting | CPU/device owners share a checked-size formatting helper while retaining their logging/failure policies. |
| Stale includes and formatting | Removed unused extraction-era Tile/network/config includes and formatted extracted controllers/configuration/reporting with the checked-in style; fresh warning-free build and full comparison passed. |

The removals are in `sst/network/wormholeNetworkInterface.{h,cc}` and
`sst/memory/globalRAMController.cc`; central validation and checks are in
`sst/configuration/` and `sst/tests/run-configuration-test.py` with
`tile-defaults.json`. These are scoped removals, not removal of optional hardware.

## Current ownership and measurement boundary

R0–R5 established the comparison copy, isolated build/provenance, configuration,
address rules and typed clocks. R6/R7 own RX/TX descriptors, queues, completions,
lanes and FIFOs. R8 owns CPU grants, capture, accounting and replay. R9 adds
`AnalogController`, `MemoryAccessController`, `GlobalDMAClient`, and
`InitializationBarrierClient`. Tile retains composition, SST lifecycle/event
dispatch and the read-only measurement assembly implemented under R10.

MemoryAccess owns the single SPM model; all clients retain shared arbitration.
Owner interfaces are typed values and narrow callbacks/resource services, with
no Tile friend/backpointer or mutable queue accessor. CPU capture stops before
borrowed resources die; `UniqueFileDescriptor` owns only the parent's duplicate.
Detailed interfaces, lifetime limits and evidence: [execution/R9.md](../../../src/sst/execution/R9.md).

R10 is implemented: named `SummarySnapshot`, JSON schema
`mittens.summary` version 1 and preserved legacy CSV. Named read-only
`TileMeasurementSnapshot`/`TaskTrace` integration and positional-call cleanup
are complete. Explicit units, availability, provenance and aggregation metadata
pass the final measurement gates.
Unavailable values remain null in JSON; legacy CSV payloads do not establish
measurement availability. Resource-owned counters stay with their resources;
schema adoption does not fix the known TX sampling/coverage limitations.

## Timing corrections and validation status

R5a intentionally corrected global-DMA CPU-cycle/tick conversion and deadline
rechecks. R5b intentionally added CPU delivery/SPM-service deadlines, deferred
early completion and harmless stale scheduled wakes. They are not equivalent
to the buggy reference timing: R5b's two existing DMA groups retain reviewed
20 summary-field/12 observation-file differences. Correctness oracles, including the
cacheless buffered-store fixture, preserve failing before evidence rather than
accepting the bug. See [the workspace guide](source-refactor-notes.md) and
[the invariants](../refactor-invariants.md).

R9's recorded build `build/src2/build-sst-1788564941613897065.json` includes the
R11 cleanup and R10 writer foundation. Its full post-R5b matrix passed 52/52 arms
with zero counter/ordered-observation differences at
[`1788564959235621218`](../../../build/src2/regressions/1788564959235621218/comparison.json).
The R9 report also records focused clock/SPM, ready-set, device-owner/sanitizer,
deadline and host gates; Main recorded all 28 configuration checks passing.
These results validate that stage, not subsequent R10 Tile integration.

Final acceptance used a fresh coordinated build, both paired matrices, the
deadline/configuration/owner/ready-set gates and 712 valid measurement records.
Earlier failed build/test records remain evidence; they are not relabeled PASS.
No default-source switch or compiler/model performance result is claimed.

Do not remove optional hardware capabilities, compiler-facing ABIs, live
profiling controls, sparse global-RAM backing, or compatibility ports solely
because the current studies do not use them. Public router `PortCount` can remain
a documented output-port alias; multi-TX local inputs have a separate count.

Further formatting or include cleanup likewise needs a fresh build and matching
hardware/timeline evidence; visual inspection alone is not an acceptance gate.
