# Mordred mesh

[Mordred](https://github.com/tactcomplabs/mordred) supplies the routers, XY mesh
routing, virtual-channel allocation, switch arbitration, credit flow control,
and `SST::Interfaces::SimpleNetwork` NICs. Its source lives in this component
directory, pinned to `ff89f33ed01b43d2a72c865fbb0faeb3bb7dcf93`.
[UPSTREAM.json](UPSTREAM.json) records provenance and file hashes;
[upstream/LICENSE.txt](upstream/LICENSE.txt) retains the Apache 2.0 notice.
The vendored source is unchanged. Our build helper replaces upstream's install
workflow and builds the raw-link transport without optional Prydwen channels.

## Build and run

From the repository root:

```sh
python3 -B src/components/mordred/build.py
python3 -B src/tests/mordred/test_configuration.py
python3 -B src/tests/mordred/run.py
```

The standalone build writes `build/src-mordred/libmordred.so` and a
source/build manifest. The main `src/build.py` also builds `libmordred.so`
beside `libtilecomponents.so`. Nothing is installed or registered globally.
Runs select the library with SST's `--add-lib-path` option.

The runtime suite builds its own payload-checking endpoint library, runs seven
2×2 configurations, and writes `tests/results/source-new-mordred/<run>/`.
Each run includes topology JSON, SST statistics/logs, per-endpoint results,
and a combined `validation.json`. See the [test contract](../../tests/mordred/README.md).

## Composition

For complete RISC-V/SPM/array tiles, use
[`connect_riscv_mesh`](spm-interface.md). Each tile's router interface accesses
an explicit list of physical SPM banks, sharing their ports and channels with
the CPU. The mesh defaults to four banks, CPU `[0,1,2,3]`, and router `[2,3]`.

With `src` on Python's import path:

```python
from components.mordred.configuration import MeshParameters, connect_mesh

# Four SST endpoint components, each exposing a SimpleNetwork networkIF slot.
network = connect_mesh(sst, MeshParameters(), endpoints=endpoints)
```

The helper attaches one `mordred.mordredNIC` to each supplied endpoint and
connects the four routers:

```text
endpoint2──router2────router3──endpoint3
              │          │
endpoint0──router0────router1──endpoint1
```

Router ID is `y * x_dim + x`. With multiple local ports, endpoint ID is
`router_id * local_ports + local_port`. Pass endpoints in that order.
Directional router ports are north/east/south/west = 0/1/2/3; local ports start
at 4. Mesh boundaries have no wraparound links. A unique `name` allows multiple
independent meshes in one simulation. Returned objects include the routers,
topologies, NICs, and links for further composition/statistics.

| Parameter | Default | Meaning |
|---|---:|---|
| `x_dim`, `y_dim` | 2, 2 | Router columns and rows |
| `local_ports` | 1 | Endpoint NICs per router |
| `clock` | 1GHz | Same router and NIC clock |
| `link_latency` | 1ns | Latency on each router or endpoint link |
| `flit_size_bits` | 128 | Transfer unit; 16 bytes at the default |
| `num_vns` | 1 | Validated helper supports one virtual network |
| `num_vcs` | 1 | Virtual channels per virtual network |
| `router_input_buffer_flits` | 8 | Input credits per router VC |
| `router_output_buffer_flits` | 8 | Output buffering per router VC |
| `nic_input_buffer_bytes` | 4096 | NIC receive flit-credit window |
| `nic_output_buffer_bytes` | 4096 | NIC packet admission/transmit buffering |

Packets occupy at least two flits (HEAD and TAIL). The NIC output buffer must
fit an entire packet rounded up to flits; check `spaceToSend()` before sending.
At the defaults a link can carry one 16-byte flit per router/NIC cycle, before
contention, credits, and packet overhead. This is independent of RVV VLEN and
the CPU-to-array link width.

## Validated scope and model limits

The network-only fixture has four test endpoints. The separate
[complete-tile fixture](spm-interface.md) attaches four RISC-V/SPM/array tiles through
bounded router-facing SPM interfaces. Neither fixture adds a direct array-to-SPM or
array-to-network path.

Tests cover one SST rank/thread, one VN, one or two VCs, all 12 directed remote
endpoint pairs (including diagonals), packet fragmentation, and finite
transmit/router-buffer backpressure. Every payload is checked before the
simulation is permitted to finish. End-to-end latency uses endpoint timestamps.

Two upstream limitations matter for later performance studies:

- NIC receive credits return when flits arrive, before the endpoint consumes
  the completed packet. The completed-packet queue is unbounded; the NIC input
  buffer is not a bound on unread application data. Slow-receiver backpressure
  is therefore not modeled accurately by this upstream transport alone. The
  SPM request/response protocol adds bounded outstanding requests retired after
  bank completion; its finite sender window bounds reachable receive traffic.
- The NIC can emit a flit for each VN in one clock and shares its head timestamp
  across VNs. The composition helper currently accepts only one VN. Upstream's
  `average_noc_latency` also assumes a fixed core-time conversion; use the
  independently measured endpoint results for this fixture.
