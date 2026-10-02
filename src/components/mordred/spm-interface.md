# Router-facing scratchpad interface

Each tile has one `tilecomponents.MordredSpmEndpoint`, named `router_spm` in
composition. It implements the [guest whole-message interface](network-instructions.md).
Its `memory` StandardMem interface shares the banked SPM controller, ports and
channels with QEMU. Its `networkIF` SimpleNetwork interface connects to a Mordred
router's local port. The CPU connects through `network_commands`.

```text
CPU / RVV ⇄ analog accelerator
    ⇅
shared banked SPM
    ⇅
NIU ⇄ NIC ⇄ mesh router
```

The NIU captures a send descriptor, reads its local source payload through timed
SPM service, and packetizes the whole message. Mordred XY routes packets directly
to the final destination; intermediate CPUs and SPMs do not participate. The
receiving NIU writes only the destination slots reserved by deployment data.
There are no remote reads, generic remote-write requests, response packets, or
external `requests` / `arrivals` event ports.

`net.wait` completes once the source bytes have been captured. `net.recv` acquires
a message only after all its destination SPM writes commit. `net.release` returns
ownership of that application's slot and schedules its hardware credit return.
No receiver-consumption acknowledgment is required for source completion.

## Complete tile composition

With `src` on Python's import path:

```python
from components.mordred.tiles import connect_riscv_mesh

system = connect_riscv_mesh(
    sst,
    {"spm_banks": 4, "cpu_spm_banks": [0, 1, 2, 3],
     "router_spm_banks": [0, 1, 2, 3], "spm_bank_width": 4},
    elfs=["tile0.elf", "tile1.elf", "tile2.elf", "tile3.elf"],
    memory_directory="run",
    qemu="/path/to/shared-spm/qemu-system-riscv64",
    router_parameters={"request_window": 4, "memory_queue_depth": 8,
                       "max_request_bytes": 256},
    network_transfers=[
        {"transfer_id": 7, "source_tile": 0, "destination_tile": 3,
         "receive_base": 0x90080000, "slot_capacity": 4096, "slot_count": 2},
    ],
)
```

The default mesh is 2×2 with row-major tile IDs. Each tile has independent CPU,
arrays, SPM, NIU and backing file `tileN-spm.bin`. Guest SPM addresses start at
`0x90000000`. The helper validates parameters, deployment records, input files
and backing aliases before creating components or zeroing output files.

The result contains `mesh`, `tiles`, `parameters`, `cpu_parameters` and
`router_parameters`. Each tile contains `cpu`, `arrays`, `scratchpad`,
`router_spm` and `memory_file`. Only active producers and consumers hold transfer
state; only destinations reserve receive SPM. Without deployment records,
there are no valid transfers to submit.

## Physical bank permissions

The mesh defaults to four physical banks, CPU access to all four, and router
access to banks 2 and 3. An explicit `spm_banks=N` selects all CPU banks and the
highest `min(2,N)` router banks unless explicit lists override these defaults.
The example above shares all banks to support contiguous messages.

```text
bank = floor(spm_byte_offset / spm_bank_width) % spm_banks
```

For four 4-byte banks, offsets 8–15 occupy banks 2–3; offsets 16–23 occupy banks
0–1. A router range at 8–15 is permitted by `[2,3]`, but 8–23 is rejected. Bank
lists select physical stripes, not bandwidth quotas. They do not remap addresses.
Every byte of a descriptor and source payload must be router-accessible. Every
reserved receive slot must be accessible to both the NIU and CPU/RVV. The compiler
must choose a layout and bank map that cover each contiguous message and slot.

CPU and router accesses share physical ports and channels. Disjoint byte ranges
can still contend for a bank. Disjoint banks can run concurrently within channel
capacity. CPU bank restrictions also apply to instruction-cache fills.

The controller and backend check exact requestor identities and bank permissions.
The controller protects overlapping accesses, including QEMU stores whose
functional commit follows their timing response. The NIU has no direct backing
file access. Arrays retain only their CPU vector-register command connection.

## Finite queues and credits

| NIU parameter | Default | Meaning |
|---|---:|---|
| `request_window` | 4 | Payload chunks across local reads, captured buffers and NIC admission |
| `max_request_bytes` | 256 | Maximum payload bytes per data packet |
| `memory_queue_depth` | 8 | Outstanding timed StandardMem fragments |
| `posted_receive_slots_per_source` | 16 | Reserved packet slots per remote source; zero disables message traffic |
| `posted_credit_batch` | 4 | Maximum packet-slot credits per return packet; capped to receive slots |
| `posted_credit_delay_cycles` | 4 | Delay before returning a partial packet-credit batch |
| `net_command_queue_depth` | 4 | Active whole-message sends |
| `net_ticket_capacity` | 16 | Live send tickets, including unretired completions |
| `net_transfers` | `[]` | Deployment records for active producers and consumers |

One additional local request slot is reserved for descriptor capture, so payload
backpressure cannot prevent an admitted send instruction from returning. Packet
windows, memory fragment queues, command queues and application slots are separate
capacities. None adds SPM ports or channel bandwidth; CPU LSQ depth is independent.

Three credit mechanisms protect different resources:

- Mordred hop-by-hop credits protect router flit buffers.
- NIU packet credits reserve storage from source NIC admission through destination
  SPM commit. Committing a packet frees that packet slot and schedules credit return.
- Application-slot credits reserve deployed destination SPM until `net.release`.
  Held input B cannot consume the storage reserved for input A.

The `posted_*` parameter names control packet credits, not an additional transport
mode. Each receiver bounds payload storage by
`(tile_count - 1) * posted_receive_slots_per_source * max_request_bytes`.
Transient StandardMem copies and metadata are separate. Source payload buffering
is bounded by `request_window`. Simulation lifetime includes outstanding credits,
even after all CPUs exit.

## Packet sizing and ordering

Every data packet uses one **96-byte header** containing transport, transfer,
invocation, sequence and slot metadata. Sizes round up to whole flits, with a
two-flit minimum. The NIC output buffer must hold the maximum rounded data packet.
For 256-byte payloads and 128-bit flits, that requires **352 bytes**, including
when no transfers are configured. There is no 40-byte compatibility format.

Packet-credit headers are 16 bytes; application-credit headers are 80 bytes.
Both consume real network bandwidth and obey the same flit rounding. Untimed
initialization advertisements consume no simulated cycles.

Payload accesses split at `spm_request_bytes` boundaries and share timed memory
service. Multiple VCs may reorder packets; the NIU restores per-source packet
order in its bounded receive slots before SPM admission. Acquisition preserves
submission order within each transfer ID; different transfers progress independently.
A multi-fragment write is not atomic relative to arbitrary application accesses.

`MORDRED_SPM_STATS` reports local SPM reads/writes, data packets in
`requests_sent` / `requests_received`, packet-credit traffic, queue maxima and
credit stalls. `posted_accepted` counts NIC admission and `posted_committed`
counts destination packet commits. `NETWORK_STATS` reports whole-message ownership,
queue limits and application credits. Optional traces include packet sequences,
SPM fragments and message identities; no network response-packet counters remain.

## Validation

```sh
python3 -B src/tests/network-instructions/test_configuration.py
bash tests/run-all.sh --case network/guest-instructions
bash tests/run-all.sh --case platform/profiling
bash tests/run-all.sh --case memory/bank-connections
```

Configuration tests check deployment reservations, exact bank roles, complete tile
wiring, packet capacity and preservation of inputs on failure. Compiled guests
check multi-hop routing, ownership, finite credits, source reuse, shared-bank
permissions, and MVM → transfer → MVM. Profiling uses that same guest transport.
