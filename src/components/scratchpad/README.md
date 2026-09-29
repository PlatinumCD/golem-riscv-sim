# Scratchpad

The functional controller and byte storage come from SST Elements'
`memHierarchy.Scratchpad`. `BankedBackend` extends its `SimpleMemBackend` API
to schedule the SRAM banks and channels. There is no second SPM store.

## Service

```text
bank = floor(byte_address / spm_bank_width) % spm_banks
```

Every served beat consumes a bank port and a channel in the same cycle. Reads
and writes have separate bank-port counts but share channel capacity. A request
can use at most one channel per cycle; a channel serves at most one request per
cycle. Requests can touch multiple banks. Least-recently-served requests win
arbitration, with arrival order breaking ties.

The default geometry is eight 32-bit (4-byte) banks, with one read and one
write port per bank and two 32-byte/cycle channels. An aligned 32-byte read
uses all eight read ports through one channel in one service cycle. A second
32-byte read must wait for those ports even if the other channel is free.
Reads and writes can use their separate bank ports concurrently, subject to
channel availability. These are backend service rates; CPU request/response
latency also includes transport and controller timing.

The QEMU CPU supplies eligible contiguous RVV accesses as preauthorized beats
up to VLEN/8 bytes. It splits these beats at the configured request-line
boundaries and allows their fragments to reach the bank scheduler together.
Thus vector accesses exercise the same bank/channel limits as other clients;
grouping does not add bank ports or bypass contention. The exact byte ranges
accessed by the CPU are retained through the whole beat's functional commit.

A partial-width access still consumes a bank port. For example, a 32-byte
channel servicing a 64-byte-wide bank transfers 32 bytes while occupying that
port for the cycle. There is no implicit bank-side FIFO carrying the unused
half of a wider read into another channel.

All memory clients share the `spm_channels` service channels. Analog arrays
have no scratchpad interface, request classification, or reserved channel pool.
Only the CPU and explicitly connected StandardMem clients access this backend.

## Physical bank access

`cpu_spm_banks` and `router_spm_banks` are lists of actual bank IDs. Standalone
composition defaults to CPU access to every bank and no router banks. The
complete tile-mesh helper defaults to four banks, CPU `[0,1,2,3]`, and router
`[2,3]`. With four-byte banks, offsets 8–15 select banks 2–3, offsets 16–23
select banks 0–1, and offsets 24–31 select banks 2–3 again. A contiguous router
request crossing from offset 15 to 16 is forbidden under that configuration.
The interface does not pack the allowed banks into a new address space.

The lists do not change the number of banks, per-bank ports, channel count or
service width. CPU and router accesses to a shared bank consume the same
physical resources and contend through the existing arbitration. Distinct
bank accesses can proceed concurrently when channels and ports permit.

Composition supplies exact `cpu_requestor` and `router_requestor` names to both
the controller and backend. The controller checks every byte's bank before
functional backing access; the backend independently checks admission. CPU
instruction-cache fills are subject to the CPU bank list, just like data
accesses. Restricting CPU banks therefore also requires code/data placement
and fetch granularity compatible with those banks.

When a router requestor is bound, unclassified clients are rejected rather
than receiving implicit all-bank permission. Existing standalone fixtures with
no router binding retain unrestricted access for their generic memory peers;
a named CPU still uses its CPU bank list. These policies control connectivity
and access, not reserved bandwidth. See the
[router-facing interface](../mordred/spm-interface.md) for remote requests.

Ordinary StandardMem fixtures can bind `cpu_requestor` and `router_requestor`
through `connect_scratchpad()` without a QEMU external-commit owner. Router
bindings enable the controller's exact-byte-range ordering in these fixtures
too. Disjoint byte ranges can reach different banks concurrently even when
they share a transport-line boundary; overlapping requests retain order.

Accepted requests retain their slot until every byte is serviced and the fixed
bank latency has elapsed. A full 64-entry backend returns retry without reserving
ports or charging bytes. SST's converter retains and retries the request.
The utilization studies can override this admission limit with the explicit
`experimental.queue_entries` composition control. It changes neither bank
capacity nor channel bandwidth; ordinary configurations retain 64 entries.

## Upstream integration

The pinned upstream controller acknowledges ordinary writes before its backend
finishes. The build-local adaptation in [prepare_controller.py](prepare_controller.py)
uses the controller's existing outstanding-request and same-line ordering
machinery for write completions as well as reads. It also forwards `finish()`
to the backend and rejects remote/out-of-range read/write requests. Cached
clients are rejected.

The adapted controller is registered separately as `tilecomponents.Scratchpad`.
The upstream source/license is retained in generated build output. The installed
`memHierarchy.Scratchpad` and `third_party` files are not modified. This is a
narrow source adaptation of the upstream controller, not a claim that its
original write-completion behavior already met our needs.

The [RISC-V composition](../riscv-qemu/README.md) uses this controller with shared
mmap backing. Its explicit `external_write_requestor` identifies CPU writes
whose values QEMU commits after the timing response. Those requests still use
the normal ordering and bank service paths; the controller skips their missing
payloads. Other clients continue to access the same functional backing bytes.
In this composition, the controller checks every client's actual byte range
before admission. Disjoint requests can proceed concurrently, including two
16-byte requests within the same 32-byte transport boundary. Overlapping
requests wait for older active requests and older overlapping waiters. CPU
ranges remain protected until the explicit post-access commit; ordinary peer
ranges remain protected through their functional backend completion. This
prevents queued clients from overtaking QEMU during response transit while
allowing independent accesses to reach the bank scheduler.

[ExactConvertor](exactConvertor.cc) extends SST's converter to preserve the real
byte address and size of short requests. `spm_request_bytes` sets the maximum
request size and transport fragment boundary (default 32 bytes).
It must be a power of two from 4 bytes through 2 MiB. Requests stay within one
such boundary, including when they are smaller than the maximum. Ordering is
independent of this width: the controller tracks exact requested bytes when
the QEMU CPU or a named router interface is connected. Unbound standalone memory fixtures retain the upstream
noncacheable ordering key (the request's starting address). Bank mapping still
uses `spm_bank_width`, not the request size.

## Evidence

Run `python3 -B src/tests/run.py` for memory and independent array
fixtures, and `src/tests/riscv-qemu/run.py` for CPU integration. CSV
traces record acceptance, each bank/channel service beat and actual completion.
The checkers enforce port/channel limits, address mapping, byte conservation,
and completed stores. Vector-analog integration tests verify that the actual
SST graph contains no array memory interface or link to SPM.
`src/tests/range-ordering/run.py` checks disjoint byte ranges, partial
overlaps, delayed external commits, peer ordering, and multi-fragment accesses.
