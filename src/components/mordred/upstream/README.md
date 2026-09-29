# Mordred - SST Network-on-Chip Library

Mordred is a modular, packet-switched Network-on-Chip (NoC) simulation library for the
[Structural Simulation Toolkit (SST)](http://sst-simulator.org/). It provides a router,
endpoint NIC, and topology implementations as drop-in alternatives to Merlin, with a
pluggable physical-channel layer that allows the transport between routers and endpoints
to be swapped independently of the routing and flow-control machinery.

An introductory presentation from the 2025 SST User's Group meeting is available [here](SST-UG2025-Mordred.pdf).

## Requirements

- CMake >= 3.19
- SST and SST-Elements (including Merlin) >= 15.0
- C++17 compiler
- `sst` and `sst-config` must be on `PATH`
- **Prydwen** must be built and registered before building Mordred (Mordred's build
  system references `prydwen/src/` for the `PhysChannelAPI` header)
  - Mordred can be built without Prydwen; add the `-DMORDRED_ENABLE_PHYS_CHANNEL=OFF` to your cmake command
    line when building mordred.

## Building

Build prydwen first, then mordred:

```bash
# 1. Build prydwen (physical channel library)
cd ../prydwen
mkdir build && cd build
cmake ..
make -j
make install

# 2. Build mordred
cd ../../mordred
mkdir build && cd build
cmake ..
make -j
make install
make test
```

## Repository Layout

```
src/             Main library source
tests/           SST Python test scripts
repotest/        Sandbox scripts for experiments and performance comparisons
merlin_testing/  Scripts comparing Mordred and Merlin behavior
sst_test_framework/  Files for running tests via the standard SST test framework
```

### `repotest/` folder

This folder is a sandbox for scripts and tests under development, performance comparisons, etc. No promises are made as to completeness or correctness. Most scripts are copy/edit variants of scripts from `tests/`.

Two historical notes:
- An early component `mordred.test_ep` served as a stand-in for `merlin.test_nic`. It has since been removed/replaced, so some older scripts in this folder may fail.
- Several scripts reference `merlin.clocked_offered_load`, a local (non-upstreamed) component based on `merlin.offered_load` that generated traffic using a clock rate rather than a bandwidth parameter. Its behavior was equivalent to `merlin.offered_load` when `link_bw` was set to match the Mordred network's link bandwidth.

### `sst_test_framework/` folder

Contains files for executing the tests via the standard SST elements test framework.  Most of the documentation for this
approach can be found [here](https://sst-simulator.org/sst-docs/docs/guides/dev/testframework).

---

## Components

### Router: `mordred.mordred_router`

The top-level SST component representing one router in the network. It owns the topology,
per-port control subcomponents, a VC allocator, and a crossbar arbiter.

**Parameters**

| Parameter        | Description                                        | Default |
|------------------|----------------------------------------------------|---------|
| `id`             | Router ID (must follow topology numbering)         | —       |
| `clock`          | Clock frequency (e.g. `"1GHz"`)                   | —       |
| `num_ports`      | Total number of ports (routing + local combined)   | —       |
| `num_local_ports`| Number of ports connected to endpoints             | —       |
| `num_vns`        | Number of virtual networks                         | —       |
| `num_vcs`        | Virtual channels per VN                            | —       |
| `flit_size`      | Flit width (e.g. `"16b"`, `"64b"`)                | —       |
| `input_buf_size` | Per-VC input buffer size (e.g. `"8"` flits)        | —       |
| `output_buf_size`| Per-VC output buffer size                          | —       |
| `verbose`        | Verbosity level                                    | `0`     |

**Subcomponent slots**

| Slot           | API                       | Required |
|----------------|---------------------------|----------|
| `topology`     | `TopologyAPI`             | Yes      |
| `portcontrol*` | `RtrPortControlAPI`       | Yes      |
| `vcalloc`      | `VcAllocAPI`              | Yes      |
| `xbarArb`      | `XbarArbAPI`              | Yes      |

---

### Endpoint NIC: `mordred.mordredNIC`

An `SST::Interfaces::SimpleNetwork` subcomponent that connects an endpoint (e.g.
`merlin.test_nic`, `memHierarchy.MemNIC`) to a Mordred router over a raw SST link.
Handles packet-to-flit fragmentation, credit-based flow control, and reassembly.

Minimum packet size is 2 flits (one HEAD flit carrying the destination and a TAIL flit
carrying the payload).

**Parameters**

| Parameter        | Description                                              |
|------------------|----------------------------------------------------------|
| `clock`          | Interface clock frequency                                |
| `input_buf_size` | Input buffer size in bits/bytes (SI prefix supported)    |
| `output_buf_size`| Output buffer size                                       |
| `verbose`        | Verbosity level                                          |

**Statistics:** `packets_recv`, `average_noc_latency`, `average_packet_size`

---

### Endpoint NIC (PhysChannel-backed): `mordred.mordredNicPC`

Identical external interface to `mordredNIC` but uses an inner `PhysChannelAPI`
subcomponent (slot `port_iface`) instead of a raw SST link. This makes the physical
transport between the NIC and router pluggable — swap in `prydwen.genericPhysChannel`
for a simple raw-link transport, or `prydwen.uciePhysChannel` (from the prydwen repo)
for a full UCIe adapter-layer transport.

**Additional subcomponent slot**

| Slot         | API              | Description                                     |
|--------------|------------------|-------------------------------------------------|
| `port_iface` | `PhysChannelAPI` | Physical channel to the router (e.g. `prydwen.genericPhysChannel`) |

---

### Test Endpoint: `mordred.mordredTestEP`

A configurable traffic-generator component for mordred test scenarios that require
`num_vns > 1`. `merlin.test_nic` and `merlin.trafficgen` both hardcode `num_vns=1`
when loading their `networkIF` subcomponent, making them incompatible with
`prydwen.uciePhysChannel` in `num_stacks=2` configuration. `mordredTestEP` accepts
`num_vns` as an explicit parameter and correctly propagates it to the networkIF loader.

Each endpoint sends `num_messages` messages, rotating through VNs 0..`num_vns-1` and
cycling through destinations, then calls `primaryComponentOKToEndSim()`.

**Parameters**

| Parameter       | Description                                              | Default |
|-----------------|----------------------------------------------------------|---------|
| `id`            | Endpoint ID                                              | `0`     |
| `num_peers`     | Total endpoints in the network                           | `1`     |
| `num_vns`       | VNs to request from the `networkIF` subcomponent         | `1`     |
| `num_messages`  | Total messages to send                                   | `10`    |
| `message_size`  | Message size (UnitAlgebra; bits or bytes)                | `64b`   |
| `clock`         | Clock frequency                                          | `1GHz`  |

**Subcomponent slot:** `networkIF` — accepts any `SST::Interfaces::SimpleNetwork` implementation.

---

### Port Control: `mordred.rtrPortControl`

Per-port subcomponent of `mordred_router`. Manages the raw SST link to the attached
router or endpoint, input/output VC state machines, credit tracking, and flit
buffering. Coordinates with the VC allocator and crossbar arbiter through shared
per-router data structures.

---

### Port Control (PhysChannel-backed): `mordred.rtrPortControlPC`

Same as `rtrPortControl` but uses a `PhysChannelAPI` subcomponent (slot `port_iface`)
for the physical link, matching `mordredNicPC` on the endpoint side.

---

### Physical Channel API: `SST::Mordred::PhysChannelAPI`

> **Moved to the [prydwen](../prydwen) repo.** Concrete implementations and the
> abstract API now live in `prydwen/src/`. Mordred references `prydwen/src/` at
> build time via `PRYDWEN_SRC_DIR` in `src/CMakeLists.txt`.

Available implementations (registered in the prydwen element library):

| SST name                   | Library  | Description                                              |
|----------------------------|----------|----------------------------------------------------------|
| `prydwen.genericPhysChannel` | prydwen | Raw `SST::Link` transport via `PhysChannelLinkEvent`     |
| `prydwen.uciePhysChannel`   | prydwen | Full UCIe adapter-layer with FLIT serialization + credits |

---

## Topologies

All topologies are subcomponents registered under the `TopologyAPI`. Endpoints must be
connected to **local ports only** — do not connect endpoints to routing ports.

All routers in a simulation should have the same number of local ports. Unconnected
local ports are permitted.

### 2D Mesh — `mordred.MeshTopology`

Rectangular grid without wrap-around.

Router ID = `(y × xDim) + x`, where `(x=0, y=0)` is the bottom-left corner.

**Parameters:** `xDim`, `yDim`, `num_local_ports`, `num_vns`, `num_vcs`

### 2D Torus — `mordred.torusTopo`

Mesh with wrap-around in both dimensions. Requires at least 2 VCs per VN to avoid
deadlock on larger networks.

Router ID = `(y × xDim) + x`

**Parameters:** `xDim`, `yDim`, `num_local_ports`, `num_vns`, `num_vcs`

### 3D Torus — `mordred.torus3DTopo`

Extension of the 2D torus with a Z dimension.

Router ID = `(z × (xDim × yDim)) + (y × xDim) + x`

**Parameters:** `xDim`, `yDim`, `zDim`, `num_local_ports`, `num_vns`, `num_vcs`

### Flattened Butterfly — `mordred.flattenedButterfly`

K-ary N-fly topology. A helper class in `tests/flatbutterfly_k2n4_testnic.py` handles
router and endpoint naming/numbering.

**Parameters:** `k`, `n`, `num_local_ports`, `num_vns`, `num_vcs`

### Cross-Domain Gateway — `mordred.GatewayTopology`

Decorates any other `TopologyAPI` implementation (`MeshTopology`, `torusTopo`,
`torus3DTopo`, `flattenedButterfly`) to join two or more independently-addressed
topology **domains** into one shared global id space — e.g. two meshes joined by a
single UCIe link, a mesh and a torus, or three domains laid out as a triangle or
chain. Each domain owns a contiguous slice `[id_base, id_base+local_range_size)`;
parallel `remote_*` lists describe every other domain directly reachable from here and
which local router+port reaches it. A destination outside a router's own slice is
matched against these entries and redirected toward that entry's gateway router,
which alone forwards it out an extra, additive port — never a repurposed
cardinal/wraparound port, since several inner topologies (`torusTopo`, `torus3DTopo`)
fatal in `init()` if any of their own router-router ports is left unconnected.

This also gives multi-hop transit for free, with no address translation: a domain
that isn't directly linked to some other domain can still reach it by naming that
domain's range with a `gateway_rtr_id`/`gateway_port` pointed at whichever neighbor
*is* directly linked. Routing is purely dest-based at every hop, so the neighbor
forwards the packet on using its own accurate, direct entry, with no awareness the
packet actually originated further away.

Untimed broadcasts relay the same way — out every gateway port a router hosts except
the one the broadcast just arrived from — made cycle-safe by a domain-wide (not
per-router) dedup keyed by `id_base`. This is needed because a domain graph with a
cycle (e.g. a triangle) can enter the same domain via two different physical gateway
routers, each its own `GatewayTopology` instance with no other shared state.

**Parameters**

| Parameter               | Description                                                                          | Default |
|-------------------------|----------------------------------------------------------------------------------------|---------|
| `id_base`               | Offset of this domain's slice in the shared global id space                            | `0`     |
| `local_range_size`      | Total endpoints this domain's inner topology owns (e.g. `xDim*yDim*num_local_ports`)   | —       |
| `remote_id_bases`       | Comma-separated `id_base` of each directly-reachable remote domain                     | `""`    |
| `remote_range_sizes`    | Comma-separated size of each remote domain (parallel to `remote_id_bases`)              | `""`    |
| `remote_gateway_rtr_ids`| Comma-separated LOCAL `rtr_id` of the gateway router to each remote domain              | `""`    |
| `remote_gateway_ports`  | Comma-separated port index the gateway router uses for each remote domain's link       | `""`    |
| `verbose`               | Verbosity level                                                                         | `5`     |

**Subcomponent slot:** `inner_topology` — the real `TopologyAPI` being wrapped.

**Known limitations:**
- Broadcast relay reaches every domain reachable via some path of directly-linked
  gateways; the dedup state that makes this cycle-safe is a plain static map (not an
  SST SharedObject), so it is not rank-safe (MPI) and not checkpointed.
- A router's gateway ports must be the trailing contiguous block of its port indices
  (enforced with a fatal at construction if misconfigured).

See `tests/mesh2x2_gateway_ucie_testnic.py` (two meshes), `tests/torus3D_3x3x3_2vc_gateway_ucie_testnic.py`
(two tori), `tests/mesh2x2_triangle_gateway_ucie_testnic.py` (three domains, fully
connected), and `tests/mesh2x2_chain_gateway_ucie_testnic.py` (three domains,
multi-hop transit, no direct link between the two end domains) for worked examples,
plus their `*_untimed_broadcast` counterparts.

---

## Allocators and Arbiters

| Component              | SST name                  | Description                                   |
|------------------------|---------------------------|-----------------------------------------------|
| `VcAllocRR`            | `mordred.vcAllocRR`       | Round-robin virtual channel allocator         |
| `XbarArbRR`            | `mordred.xbarArbRR`       | Round-robin crossbar arbitration              |

---

## Notes on VN and VC

The topology defines the number of VCs per VN, so VC count is a topology-level property, not a router-level one. Inside the router, the total VC count (`num_vcs`) is the sum across all VNs. Most data structures are multi-dimensional arrays contained within a port (or a per-port object) where one dimension is the number of VNs and another is the number of VCs.

---

## Basic Software Architecture / Router Behavior

The router owns a vector `perPortSharedObjs` (one element per port), where each element is a `RtrOwnedSharedObjs` (defined in `MordredEvents`). `RtrOwnedSharedObjs` contains a pair of 2D vectors: `needVcAlloc` and `needSwitchAlloc`.

On each clock tick, `RtrPortControl` inspects the state of each (VN, VC) pair it owns. If the flit in a pair needs an output VC, `needVcAlloc` for that pair is marked. The VC allocator can then operate on any/all pairs that need allocation. When it grants an output VC, it clears `needVcAlloc` for that pair. This allows requests to persist across clock cycles, and lets the VC allocator reason about all pending requests simultaneously (across ports, VNs, and VCs).

The same pattern applies for switch allocation via `needSwitchAlloc`.

Currently `MordredRouter` performs switch allocation on a per-packet basis, but the design should support per-clock-tick allocation (this should be tested).

---

## Flow Control and Initialization

Credits are exchanged during SST's `init()` phases before simulation begins. One credit
represents one flit of buffer space. The initialization handshake runs strictly between
each `MordredNIC` (or `MordredNicPC`) and the `RtrPortControl` (or `RtrPortControlPC`)
of the directly connected router port; no global broadcast occurs.

| Phase | MordredNIC                              | RtrPortControl                                                   |
|-------|-----------------------------------------|------------------------------------------------------------------|
| 0     | Send: report endpoint                   | Send: report router, router ID, port number                      |
| 1     | Recv: router ID and port number         | Recv: connection type; if endpoint, send num VNs, num VCs, flit width |
| 2     | Recv: num VNs, num VCs, flit width      | If endpoint: send endpoint ID                                    |
| 3     | Recv: endpoint ID                       | Idle                                                             |
| 4     | Exchange credits                        | Exchange credits                                                 |
| 5+    | Receive credits; enqueue other events   | Receive credits; enqueue other events                            |

---

## Compatibility with Merlin

`mordred.mordredNIC` fills the `SST::Interfaces::SimpleNetwork` subcomponent slot, so
it works with any component that accepts a `SimpleNetwork` interface:

- `merlin.test_nic` — general-purpose test endpoint
- `memHierarchy.MemNIC` — see `tests/ipdps25tutorial_demo7.py`
- `memHierarchy.MemNICFour` — see `tests/mordred_memNICFour.py`
- `merlin.Bridge` — see `tests/mordred_testBridge.py`

To replace a `merlin.hr_router` with a single-router deployment, use a 1×1 mesh
topology. Links that previously connected to `hr_router` become links to the local
ports of `mordred.mordred_router`.

---

## Tests

Tests live in `tests/` and are run via `make test` from the build directory.

**Standard tests** (always registered):

| Test script                                  | Topology           | Notes                                                                  |
|----------------------------------------------|--------------------|------------------------------------------------------------------------|
| `mesh3x3_testnic.py`                         | 3×3 mesh           | Baseline: 1 VC, 1 VN, 16-bit flits, 1 local port per router           |
| `mesh3x3_untimed_broadcast.py`               | 3×3 mesh           | `send_untimed_broadcast=true`; exercises `MeshTopology::routeUntimedBroadcastPacket` |
| `mesh3x3_2local_testnic.py`                  | 3×3 mesh           | Concentration=2 (2 local ports per router); exercises non-zero `dest - endptZeroId` final-hop offset |
| `mesh3x3_single_flit.py`                     | 3×3 mesh           | 1-flit request → 2-flit minimum packet; HEAD immediately followed by TAIL, empty body loop |
| `mesh3x3_large_message.py`                   | 3×3 mesh           | 16-flit packets; stresses body-flit send loop and output-buffer backpressure |
| `torus5x5_2vc_testnic.py`                    | 5×5 2D torus       | 2 VCs required for deadlock avoidance on 5×5                           |
| `torus5x5_2vc_untimed_broadcast.py`          | 5×5 2D torus       | `send_untimed_broadcast=true`; exercises `TorusTopo::sendBroadcast` wrap-aware flood logic |
| `torus5x5_3vc_testnic.py`                    | 5×5 2D torus       | 3 VCs; exercises `VcAllocRR` beyond the minimum deadlock-free VC count |
| `torus3D_3x3x3_2vc_testnic.py`              | 3×3×3 3D torus     | 2 VCs                                                                  |
| `torus3D_3x3x3_2vc_untimed_broadcast.py`    | 3×3×3 3D torus     | `send_untimed_broadcast=true`; exercises `Torus3DTopo::sendBroadcast` across 6 directions |
| `flatbutterfly_k2n4_testnic.py`             | k=2, n=4 butterfly | —                                                                      |
| `ipdps25tutorial_demo7.py`                  | Mesh               | `memHierarchy.MemNIC` integration                                      |
| `mordred_memNICFour.py`                     | Mesh               | `memHierarchy.MemNICFour` integration                                  |
| `mordred_testBridge.py`                     | Mesh               | `merlin.Bridge` integration                                            |

**PhysChannel tests** (registered only when `MORDRED_ENABLE_PHYS_CHANNEL=ON`; require prydwen):

| Test script                                  | Topology           | Transport                               |
|----------------------------------------------|--------------------|-----------------------------------------|
| `mesh2x1_rtrportcontrolpc.py`                | 2×1 mesh           | `prydwen.genericPhysChannel`            |
| `mesh2x1_uciePhysChannel.py`                 | 2×1 mesh           | `prydwen.uciePhysChannel`               |
| `mesh3x3_uciePhysChannel.py`                 | 3×3 mesh           | `prydwen.uciePhysChannel`               |
| `flatbutterfly_k2n4_uciePhysChannel.py`      | k=2, n=4 butterfly | `prydwen.uciePhysChannel`               |
| `torus5x5_2vc_uciePhysChannel.py`            | 5×5 2D torus       | `prydwen.uciePhysChannel`, 2 VCs        |
| `torus5x5_3vc_uciePhysChannel.py`            | 5×5 2D torus       | `prydwen.uciePhysChannel`, 3 VCs        |
| `torus5x5_4vc_uciePhysChannel.py`            | 5×5 2D torus       | `prydwen.uciePhysChannel`, 4 VCs        |
| `torus3D_3x3x3_2vc_uciePhysChannel.py`      | 3×3×3 3D torus     | `prydwen.uciePhysChannel`, 2 VCs        |
| `mesh2x1_uciePhysChannel_flit_format2.py`   | 2×1 mesh           | `prydwen.uciePhysChannel`, FLIT format 2 (68B/64B payload, 16 GT/s) |
| `mesh3x3_uciePhysChannel_2module.py`        | 3×3 mesh           | `prydwen.uciePhysChannel`, 2 bonded modules |
| `mesh3x3_uciePhysChannel_2stack.py`         | 3×3 mesh           | `prydwen.uciePhysChannel`, 2 UCIe stacks; uses `mordred.mordredTestEP` for multi-VN traffic |
| `mesh2x1_ucie_rtrlink_testnic.py`           | 2×1 mesh           | `prydwen.uciePhysChannel` isolated to the router-router link only; both endpoints use plain `mordredNIC` (no UCIe on the endpoint hop) |
| `mesh4x2_ucie_rtrlink_testnic.py`           | 4×2 mesh           | Same idea, scaled to a 2×2 mesh on each side of the single UCIe link (plus one necessary plain boundary link, since the two halves are one unified `MeshTopology`) |
| `mesh2x2_gateway_ucie_testnic.py`           | Two 2×2 meshes     | `mordred.GatewayTopology`; single UCIe link carries 100% of cross-mesh traffic (no second boundary link needed, since the two meshes are independently addressed) |
| `mesh2x2_gateway_untimed_broadcast.py`      | Two 2×2 meshes     | Same, with `send_untimed_broadcast=true` |
| `torus3D_3x3x3_2vc_gateway_ucie_testnic.py` | Two 3×3×3 3D tori  | `GatewayTopology` wrapping `torus3DTopo`; gateway router gets a genuinely additive port, never a repurposed wraparound link |
| `mesh2x2_triangle_gateway_ucie_testnic.py`  | Three 2×2 meshes   | `GatewayTopology`; fully-connected triangle (each domain has 2 gateway routers, one per edge) |
| `mesh2x2_triangle_gateway_untimed_broadcast.py` | Three 2×2 meshes | Triangle + broadcast; exercises the domain-wide, cycle-safe dedup |
| `mesh2x2_chain_gateway_ucie_testnic.py`     | Three 2×2 meshes   | `GatewayTopology`; A-B-C chain with no direct A-C link — A and C reach each other via multi-hop transit through B, using only configuration (aggregated `remote_*` routes), no address translation |
| `mesh2x2_chain_gateway_untimed_broadcast.py`| Three 2×2 meshes   | Chain + broadcast; exercises multi-hop broadcast relay through the middle domain |

---

## Notes and Limitations

- **Priority:** Not implemented. Use VNs for traffic class separation (SST `SimpleNetwork::Request` has no priority field).
- **Router latency:** Fixed; no configurable pipeline depth.
- **Packet size:** No maximum enforced. Minimum is 2 flits (one HEAD + one TAIL), enforced at the NIC.
- **Channel width:** Only one flit traverses a link per clock cycle. The `channel_width` branch has initial support for modifying this but is likely out of date.
- **NetworkInspectors:** Not yet supported.
- **Buffer allocation:** Buffers are fully segregated per (VN, VC) pair; no shared pool. The output buffer currently has a small buffer per (VN, VC). A configurable arbitration policy for which (VN, VC) gets access is not yet implemented; the current design is round-robin. See also the `OutputArbitration` API class in `RtrPortControlAPI.h`.
- **Arbitration:** Round-robin only for both VC allocation and crossbar arbitration.
- **Timing:** Router and subcomponent timing should continue to be reviewed.
- **Routing:** Each topology implements its own routing; no general-purpose routing algorithm API.
- **Broadcast/Multicast:** Not supported at simulation time. Untimed broadcast (SST init/complete phases) is supported and tested via the `*_untimed_broadcast` test scripts — each topology implements `routeUntimedBroadcastPacket` with its own flood logic. Across `GatewayTopology`-joined domains, broadcasts relay through the domain graph (multi-hop, cycle-safe — see [Cross-Domain Gateway](#cross-domain-gateway--mordredgatewaytopology)) as long as every domain that needs the broadcast is reachable via directly-linked gateways.
- **UCIe multi-stack (requires Prydwen) (`num_stacks=2`):** Tested via `mesh3x3_uciePhysChannel_2stack.py` using `mordred.mordredTestEP` with `num_vns=2`. `merlin.test_nic` and `merlin.trafficgen` cannot be used here: both hardcode `num_vns=1` when loading their `networkIF` subcomponent (trafficgen's `num_vns` parameter is documented but its value is ignored in the implementation), so `uciePhysChannel` always receives a VN count of 1 and rejects `num_vns_per_stack="1,1"` (total=2) as a mismatch. `mordredTestEP` was added to mordred specifically to support configurable `num_vns`.

---

## Acknowledgments

This work was supported by the U.S. Department of Energy, Office of Science, Advanced
Scientific Computing Research program under project 84245 — Democratization of
Co-design for Energy-Efficient Heterogeneous Computing (DeCoDe) at Pacific Northwest
National Laboratory (PNNL). PNNL is a multi-program national laboratory operated for
the U.S. Department of Energy (DOE) by Battelle Memorial Institute under Contract No.
DE-AC05-76RL01830.
