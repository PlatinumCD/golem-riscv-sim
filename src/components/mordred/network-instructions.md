# Guest network instructions (Xgolemnet v1)

A guest CPU submits a whole message to its local NIU. The NIU reads local SPM,
forms packets, and uses Mordred's XY routing to the final destination tile.
Intermediate CPUs, NIUs and SPMs do not forward the message. The destination
NIU commits the payload to its own SPM through the banked controller. CPU
registers carry control values only; arrays still communicate through RVV registers.

`net.send(destination_tile, descriptor)` addresses a final tile, not a direction
or a guest queue handle. `net.recv()` acquires any eligible completed message,
without selecting its source or transfer. There is no guest connection setup.
The standard composition connects each CPU to its NIU over a 1 ns control link
and uses 1 GHz clocks. This is the only tile messaging interface; standalone
remote-memory event ports are not supported.

## Instructions and descriptor

Include `guestNetwork.h` for C/C++ or `network.inc` for assembly `.S` files.
The wrappers/macros each emit one RV64 instruction using `.insn`, supported by
the installed LLVM and GNU assemblers. Built-in disassembler mnemonics are not
required. The opcode remains CUSTOM_1 (`0x2b`); function assignments are unchanged.

| Instruction | funct3 | funct7 | rs1 | rs2 | rd |
|---|---:|---:|---|---|---|
| `net.send` | 0 | 0 | Destination tile ID | Descriptor pointer | Send ticket or negative error |
| `net.recv` | 1 | 0 | `x0` | `x0` | Receive token; blocks when none is ready |
| `net.recv.try` | 1 | 1 | `x0` | `x0` | Receive token or `EMPTY` |
| `net.info` | 2 | 0 | Receive token | Field number | Metadata or negative error |
| `net.release` | 3 | 0 | Receive token | Zero | Zero or error |
| `net.wait` | 4 | 0 | Send ticket | Zero | Zero or error; retires completed ticket |
| `net.wait.try` | 4 | 1 | Send ticket | Zero | `PENDING`, or completion with retirement |

Both encoded input registers of receive are reserved `x0`. Other register
encodings trap, even if those registers contain zero. The removed setup opcode
(`funct3=5`) remains illegal. Nonzero reserved values for release/wait return
`INVALID`. A CPU without a connected NIU returns `UNAVAILABLE`.

```c
typedef struct GolemNetDescriptor {
    uint64_t source_address;  // CPU-visible local SPM byte address
    uint64_t bytes;
    uint64_t transfer_id;     // Compiler-generated connection identity
    uint64_t invocation_id;   // Compiler-generated execution/iteration identity
} GolemNetDescriptor;
```

The descriptor remains **32 bytes**, little endian, aligned to eight bytes.
Both IDs are nonnegative 63-bit values, including zero and `INT64_MAX`.
Unsupported values are rejected, never truncated. They are transported unchanged;
the NIU neither generates compiler identities nor interprets operations.
The hardware message sequence is an independent counter, starting at one per
transfer and increasing on successful submission, not on failed admission.

| `net.info` field | Number | Meaning |
|---|---:|---|
| `GOLEM_NET_POINTER` | 0 | Directly usable local SPM pointer |
| `GOLEM_NET_LENGTH` | 1 | Whole-message byte count |
| `GOLEM_NET_SEQUENCE` | 2 | Hardware sequence within this transfer |
| `GOLEM_NET_TRANSFER_ID` | 3 | Compiler transfer ID; `GOLEM_NET_TAG` remains an alias |
| `GOLEM_NET_SOURCE` | 4 | Original source tile |
| `GOLEM_NET_INVOCATION_ID` | 5 | Compiler execution/iteration ID |

Errors: `WOULD_BLOCK=-1`, `INVALID=-2`, `STALE=-3`, `RANGE=-4`, `BANK=-5`,
`PENDING=-6`, `EMPTY=-7`, `UNAVAILABLE=-8`. Unknown transfers, wrong producers,
wrong destinations, oversized messages and unsupported IDs return `INVALID`.
Messages must be nonempty, inter-tile, and fit the configured slot capacity.
Descriptors and source payloads must fit local SPM and be accessible to NIU bank
ports; these use `RANGE` or `BANK` for invalid memory. CPU-visible SPM starts at
`0x90000000`; the guest never supplies a remote memory address.

The optional [DRAM Tile](../dram_tile/README.md) extends only the local payload
source: its descriptors remain in control SPM, while `source_address` names
its configured DRAM region. Its NIU reads that region through a timed memory
controller. Compute-tile source rules, destination receive slots, packet format,
instruction encodings and completion/ownership semantics are unchanged.

Successful send captures all descriptor words before returning. Descriptor storage
can then be reused. The source payload must remain unchanged until that ticket
completes. This is a software ownership rule, not protection against arbitrary
CPU writes into a live source buffer.

## Deployment and bounded receive storage

Configure active transfers before guest execution, using compiler deployment data:

```python
connect_riscv_mesh(sst, parameters, elfs=elfs, memory_directory=output,
    network_transfers=[
        dict(transfer_id=17, source_tile=0, destination_tile=3,
             receive_base=0x90080000, slot_capacity=4096, slot_count=2),
        dict(transfer_id=23, source_tile=1, destination_tile=3,
             receive_base=0x90082000, slot_capacity=4096, slot_count=1),
    ])
```

Each transfer ID has one producer and one destination. The first record reserves
8 KiB at tile 3 for transfer 17; the second reserves a separate 4 KiB for transfer
23. A message on transfer 23 cannot consume transfer 17's slots. These reservations
are internal: both feed the same `net.recv()` interface. Unconfigured transfers
allocate no receive storage. A sending tile does not need local receive slots.

Slot capacity is also the contiguous slot stride. Receive regions must be
8-byte aligned, disjoint on each destination, within its SPM, and accessible to
both CPU and NIU banks. The deployment's memory layout must reserve them separately
from code, stack, weights and other data. Bank lists select physical interleaved
addresses, not independent bandwidth quotas.

The composition validates the full deployment before creating components or
mutating backing files. It rejects duplicate IDs/producers, missing or unknown
fields, invalid tiles, overlapping reservations and inaccessible storage.
Only producer/consumer NIUs receive each record. Their matching configurations
are checked during untimed SST initialization; guest startup has no setup
instruction, descriptor fetch or timed configuration exchange. Credits after
`net.release` still consume modeled network bandwidth.

| Capacity | Default or bound |
|---|---|
| `net_command_queue_depth` | 4 active send descriptors; 1..1024 |
| `net_ticket_capacity` | 16 active/completed tickets; at least command depth, at most 1024 |
| `network_transfers` | Empty by default; at most 1024 active transfer records |
| `slot_count` | Explicit per transfer, 1..256; at most 65536 receive slots per tile |
| `slot_capacity` | Explicit per transfer, bounded by destination SPM |

`net_transfers` is the underlying SST parameter: flat six-word records in the
same order as the deployment fields above. Use the composition's structured
`network_transfers` argument for full deployment validation. Directional receive
configuration parameters are removed.

## Ordering, ownership and progress

Submission order is preserved **within each transfer ID**. Application credits
are reserved in that order so a later message cannot own the final slot while an
earlier one waits. Messages can subsequently complete out of order. Acquisition
waits for the next sequence within that transfer; eligible transfers share a
bounded fair queue, so an incomplete transfer does not block another ready one.

Send reserves a command and ticket together. If either is full, it immediately
returns `WOULD_BLOCK`, without retaining the descriptor or waiting for capacity.
The command entry is freed after source capture, while completion state remains
until wait retires the ticket. Software must wait/retire existing tickets before
retrying a capacity failure. Receive tokens and send tickets include generations;
stale tokens, wrong token kinds and double releases return `STALE`.

Source storage for in-flight payload reads, captured chunks and packets awaiting
NIC admission is jointly bounded by `request_window`. Descriptor capture has
one additional reserved local read request so payload backpressure cannot trap
an admitted descriptor fetch. Both share normal banks, channels and
`memory_queue_depth`; there are at most `request_window + 1` local requests.
Scheduling skips transfers without application credits and peers without packet
credits. Physical links and buffers remain shared and finite.

Send observes preceding CPU/RVV stores before descriptor or payload reads. This
implementation orders all preceding stores on that CPU; it does not drain unrelated
loads or analog work. Receive publishes a token only after the entire message has
committed through timed destination SPM service. Subsequent loads can use its
pointer immediately. Metadata and storage remain stable until release.

`net.wait(ticket)` completes when the NIU no longer needs the source bytes. It
waits only for that ticket, not receiver consumption, remote commitment or
unrelated transfers. Backpressure can still delay source capture. `net.release`
frees the application slot, advances its generation, and queues a hardware credit;
it returns without waiting for a remote acknowledgment. Application-slot credits
are separate from router and posted packet-buffer credits. Simulation shutdown
also waits for outstanding credits to return, even after every CPU has exited;
this does not delay source-ticket completion or instruction retirement.

Software must finish **all readers** before release, including asynchronous RVV
loads and any forwarding send using that receive buffer. For forwarding, wait
for that send before releasing its source token. The NIU does not infer these
software dependencies or perform intermediate CPU forwarding for routing.

QEMU suspends at a pending receive/wait. SST continues clocking the NIU, SPM,
routers, credits and analog engines. Predicate checks and wait registration occur
on the same event thread, preventing a lost wakeup. No guest polling loop or
per-cycle QEMU rendezvous is needed. The bridge ABI remains version 35; rebuild
QEMU and the SST components together for this instruction contract.

## Compiler interaction

The compiler retains its transfer matching and supplies both IDs automatically.
A receiver can maintain a bounded pending-input table:

```text
token = net.recv()
transfer = net.info(token, TRANSFER_ID)
invocation = net.info(token, INVOCATION_ID)
record token under (transfer, invocation)
run an operation when its required inputs are available
release tokens after their final use
```

The NIU transports opaque identity metadata and owns buffer placement. The runtime
owns operation readiness and token lifetimes; it can omit metadata queries when
the answer is statically known. This hardware interface and its compiled guest
tests do not replace Sculptor's existing single-tile runtime adapter; lowering
that adapter to multi-tile network operations is separate compiler integration.

## Wire accounting, tests and observations

Each message packet has a single 96-byte modeled header: 40 bytes of transport
fields plus slot/reserved (two 32-bit words), generation, hardware sequence,
message offset, total length, transfer ID and invocation ID (six 64-bit words).
The IDs are charged on every packet; there is no shorter compatibility format. Application-slot credit packets are 80 bytes,
rounded to whole flits with the existing two-flit minimum. There is no payload ACK.

```sh
python3 -B src/components/riscv-qemu/build_qemu.py
python3 -B src/tests/network-instructions/run.py
# Or through the maintained correctness suite:
bash tests/run-all.sh --case network/guest-instructions
```

Real compiled guests test multi-hop XY delivery without intermediate CPU/SPM work,
multiple producers with source-agnostic receives, B-before-A with separate reserved
slots, repeated invocations, zero and maximum IDs, source reuse before consumption,
stable held buffers, queue-full admission, credit progress, and MVM → multi-hop
transfer → MVM. Other checks cover descriptor reuse, stale tokens, sequence ordering,
64 B through 64 KiB payloads, small packets, multiple VCs and reserved encodings.
Router flit counters reconcile with exact XY paths. Instruction-budget and optional
profiling comparisons check timing invariance; no studies or dashboard are required.

`NETWORK_STATS` reports configured transfers/slots, payload/descriptor bytes,
queue maxima, waits and application credits. CPU statistics report network instruction
counts and response cycles. With tracing enabled, `*-network.csv` records CPU
issue/response and `*-messages.csv` records transfer/invocation identities, capture,
commits, readiness and ownership. `TILE_CYCLE_PROFILE` adds NIU queue occupancy.
