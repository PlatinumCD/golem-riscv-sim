# Router-facing scratchpad interface

Each tile has one `tilecomponents.MordredSpmEndpoint`, named `router_spm` in
the composition. Its `memory` StandardMem interface reaches the local banked
SPM through the same controller and service resources as QEMU. Its `networkIF`
SimpleNetwork interface connects to a Mordred router's local port.

```text
                 CPU / RVV ⇄ analog arrays
                     ⇅
           SPM controller and physical banks
                     ⇅
              router SPM interface
                     ⇅
                NIC ⇄ mesh router
```

The interface has no host mmap access or separate SPM storage. A network READ
returns bytes obtained from timed bank read responses. Legacy network writes
return a completion response after their timed bank writes complete. Posted
writes release the source after reserving receiver storage and transferring
payload ownership to the NIC. They report SPM completion only at the destination.
The existing controller
ordering protects overlapping CPU and router accesses, including QEMU stores
whose functional commit occurs after the timing response. Arrays retain only
their CPU vector-register command connection.

## Complete tile composition

With `src` on Python's import path:

```python
from components.mordred.tiles import connect_riscv_mesh

system = connect_riscv_mesh(
    sst,
    {"spm_banks": 4, "cpu_spm_banks": [0, 1, 2, 3],
     "router_spm_banks": [2, 3], "spm_bank_width": 4},
    elfs=["tile0.elf", "tile1.elf", "tile2.elf", "tile3.elf"],
    memory_directory="run",
    qemu="/path/to/shared-spm/qemu-system-riscv64",
    router_parameters={"request_window": 4, "memory_queue_depth": 8,
                       "max_request_bytes": 256,
                       "posted_receive_slots_per_source": 16,
                       "posted_credit_batch": 4,
                       "posted_credit_delay_cycles": 4},
)
router_interface = system["tiles"][0]["router_spm"]
```

The default mesh is 2×2, with one complete tile per router and row-major tile
IDs. Each tile has independent CPU, arrays, scratchpad, router interface, bus,
and backing file `tileN-spm.bin`. All guest SPM physical addresses start at
`0x90000000`; each QEMU process has separate backing storage. Components and
links are qualified with `<name>.tileN.`; the default name is `tile_mesh`.
The helper checks all parameters, input files and backing aliases before
creating components or zeroing any output file.

The returned dictionary contains `mesh`, `tiles`, `parameters`,
`cpu_parameters`, and `router_parameters`. Each tile contains `cpu`, `arrays`,
`scratchpad`, `router_spm`, and `memory_file`. The router interface's optional
`requests` and `arrivals` event ports are left unconnected for callers to attach
a request producer and a receiver completion consumer. CPU/QEMU instructions
and synchronization ABI are unchanged.

## Physical bank permissions

The mesh defaults to four physical banks. CPU access includes all four, and
router access includes banks 2 and 3. An explicit `spm_banks=N` selects every
bank for the default CPU list and the highest `min(2,N)` banks for the default
router list. Explicit nonempty lists override those defaults. Generic standalone
composition retains eight banks, an all-bank CPU list, and an empty router list.

All request bytes use the existing mapping:

```text
bank = floor(spm_byte_offset / spm_bank_width) % spm_banks
```

For four banks of four bytes each, offsets 8–15 and 24–31 occupy banks 2–3.
Offsets 0–7 and 16–23 occupy banks 0–1. A router request to offsets 8–15 is
permitted by `[2,3]`; a request to offsets 8–23 is rejected because part of its
range belongs to forbidden banks. There is no bank compaction, address remap,
or separate bandwidth quota. Applications place shared data in permitted
stripes or submit separate requests for those stripes.

CPU and router accesses to shared banks consume the same physical ports and
channels. They can contend even when their byte ranges do not overlap.
Requests to disjoint banks may execute concurrently subject to the configured
channel capacity. Restricting CPU banks also restricts instruction fetches;
an instruction-cache fill touching a forbidden bank is not exempt.

The helper binds exact CPU and router StandardMem requestor names at both
controller and backend. The controller checks permissions before touching
functional storage, and the backend independently checks requests. With a
router binding active, unknown requestors are rejected. Standalone generic
memory fixtures retain their existing unrestricted peer access.

## Request and response contract

The optional `requests` SST link carries `TileComponents::MordredSpmRequest`
events from [spmRequest.h](spmRequest.h). A request contains a request ID,
source and destination tile IDs, target SPM byte offset, byte count, read/write
selection, a `posted` flag, and a payload for writes. These are **SPM offsets**,
not CPU physical addresses. READ responses carry the requested data. Legacy
WRITE responses carry completion status without a data payload.

For `posted=true`, only remote writes are valid. A source-local `Accepted`
response (status 5) means the target has reserved capacity for the complete
payload and the NIC owns the packet. The caller may then reuse its source
buffer. It does **not** mean the target SPM write has completed. Posted IDs
must be positive and strictly increase at each source. Rejections, including
`Busy`, do not advance that sequence; a caller must not retry an older ID after
successfully enqueueing a newer posted ID. Local and legacy live IDs still
must be unique at the source.

The optional destination `arrivals` link emits one local event after every
remote write commits, including legacy writes when that link is connected.
It preserves request identity, sets `response=true`, `arrival=true`,
`status=Success`, and carries no payload. A receiver must aggregate all chunks
of a vector before marking the vector ready for its CPU. A link observation
timestamp includes that local link's latency. A posted send requires a
connected destination arrival consumer; absence is rejected before acceptance.

A request targeting the interface's own tile uses a direct local-bank path:
it performs the same timed StandardMem accesses and permission checks, then
returns through `requests`, without injecting a network packet. Local-bank and
remote requests share the same finite request window and memory fragment
queue. A controller can explicitly read local source bytes, then submit a
remote write using the returned data; the interface does not schedule that
sequence itself. This does not expose remote memory operations to CPU guests.

`local_bank_completed` counts successful direct accesses;
`local_bank_rejected` counts their range/bank rejections. Malformed requests
and a full request window retain the generic `local_rejected` count.
`local_completed` continues to count received remote responses. Memory byte
counters include both kinds of bank service; packet and wire counters count
only network traffic.

Status 0 means committed success, 1 malformed or unsupported request, 2 invalid
address range, 3 forbidden bank, 4 a full local request window, and 5 an accepted
posted send. Rejected requests do not modify SPM. Receiver address bounds,
bank permissions, maximum payload, capacity and arrival-consumer presence are
advertised during untimed initialization, allowing posted requests to be checked
before acceptance even with heterogeneous tiles. A downstream failure after
acceptance is a simulation error. A multi-fragment request is not an atomic
transaction relative to arbitrary software updates.

No guest descriptor, control region, notification word, polling engine or STOP
command is required. The event-producing component schedules requests and
consumes their responses. There is no new guest network instruction in this
change. Guests use their ordinary memory instructions to access their local
SPM and observe remote writes; software synchronization remains responsible
for coordinating concurrent access. Writes to executable bytes additionally
require the CPU's normal `fence.i` before execution.

## Finite requests and service

| Router-interface parameter | Default | Meaning |
|---|---:|---|
| `request_window` | 4 | Maximum queued/live local requests; also legacy receive slots per source |
| `max_request_bytes` | 256 | Maximum bytes in one read or write request |
| `memory_queue_depth` | 8 | Outstanding target StandardMem fragments |
| `posted_receive_slots_per_source` | 16 | Reserved receiver payload slots for each remote source; 0 disables posted receives |
| `posted_credit_batch` | 4 | Maximum storage credits per return packet; capped to receive slots |
| `posted_credit_delay_cycles` | 4 | Time before a partial credit batch is eligible to return |

Tile count, SPM capacity, request boundary, bank geometry and router bank list
are derived from architecture parameters. Flit width comes from mesh settings.
The complete-tile helper uses one router local port, one VN, and a common 1 GHz
CPU/SPM/router clock. Request-window and memory-queue settings are independent
of CPU LSQ depth; none adds bank ports or channel bandwidth.

Packets have a modeled 40-byte header. Their size rounds up to whole flits and
at least two flits. The NIC output buffer must hold a maximum-size WRITE request
or READ response; composition rejects insufficient capacity. Target memory
requests split at `spm_request_bytes` boundaries and consume the existing bank
service. Legacy traffic retains its finite outstanding-request window.

Posted storage reservations cover the packet while it is queued at the source
NIC, in the network, at the receiver NIC, and awaiting destination SPM service.

Multiple virtual channels may reorder packets from one source. The endpoint
assigns each posted packet a consecutive per-destination transport sequence
at NIC admission and restores that order before admitting packets to SPM.
Out-of-order payloads occupy the existing reserved receive slots; this adds no
unbounded receive storage or source-visible completion response. Caller request
IDs remain unchanged. The sequence uses the sixth 32-bit word of the existing
40-byte header, with receiver-advertisement protocol version 2 identifying this
contract. Sequence wrap is rejected explicitly. Packet admission order does
not make a multi-fragment write atomic with respect to other memory accesses.

Trace rows include `transport_sequence`, `posted_sequence_wait`, and
`posted_admit`. Counters report reordered packets, maximum held packets per
source, and cycles awaiting a sequence gap. A one-VC comparison must reproduce
existing data traffic and timing before multi-VC performance is interpreted.

Each receiver reserves up to
`(tile_count - 1) * posted_receive_slots_per_source * max_request_bytes` payload
bytes, partitioned by source. With two tiles and defaults this is 4 KiB per
receiver; with four tiles it is 12 KiB. These are modeled finite capacities,
allocated as packets become live. Endpoint metadata and transient StandardMem
fragment copies are separate from the payload-capacity bound. Locally queued
unsent payloads are independently limited by `request_window`.

When no receiver slot is available, a queued send waits for credit without
returning `Busy` repeatedly. SPM completion frees receiver storage and returns
credits in ordinary timed network packets: a 16-byte credit header padded to
at least two flits. Partial batches flush after the configured delay. These
packets consume link bandwidth and carry a slot count, not request completion
identities; they never appear as source client responses. Mordred's existing
hop-by-hop router-buffer credits remain independent. Endpoints hold simulation
lifetime through all outstanding reservations and credit traffic.

Statistics separate `posted_accepted`, `posted_committed`, destination
`arrivals_sent`, credit packet/slot counts, credit wire bytes and credit-wait
cycles. Reservation maxima are per source/peer. Total network wire-byte and
packet counters include timed storage credits; initialization advertisements
consume no simulated cycles or timed packet counts.

## Validation

```sh
python3 -B src/tests/test_configuration.py
python3 -B src/tests/mordred-spm/test_configuration.py
python3 -B src/tests/mordred-spm/run.py
python3 -B src/tests/mordred-posted/run.py --help
```

Configuration tests check physical-bank lists, exact role names at controller
and backend, complete four-tile wiring, independent backing storage and
preservation of input files on failure. Runtime fixtures inspect functional
bytes, request/response completion, bank/port/channel limits, CPU/router
contention, and rejection of forbidden physical-bank accesses. Traces are
qualified by component name; results live under `tests/results/`.
