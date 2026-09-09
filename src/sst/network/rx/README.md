# Configurable receive concurrency

`rx_dma_streams = 1 | 2 | 4` selects independently scheduled RX DMA lanes.
The default remains 1. The mesh builder propagates this to `rx_streams` on
the wormhole router and NIC and connects `max(tx_streams, rx_streams)` local
links. TX and RX counts are independent; external cardinal link bandwidth,
routing and SPM bank/port counts do not change.

Each local ejection lane has the existing configured finite NIC receive
capacity and its own credit returns. The router selects distinct inputs for
available local outputs and holds packet ownership through the tail. The
NIC keys packet assembly by (source, packet ID), since packet IDs are not
global. Multiple receive lanes require flit-level routing; burst coalescing
is rejected for this configuration.

RX scheduling chooses the earliest available DMA lane and preserves timing
order for each source. Ordinary-memory lanes have separate DMA timing engines;
SPM lanes use NetworkReceive[0..3] client identities in the **same** scratchpad
arbiter. Bank and write-port conflicts therefore still serialize service.
The bridge burst queue remains bounded and functional completion ownership
is unchanged. This does not duplicate scratchpad capacity or write ports.

Validation:

- `studies/compute-communication/hardware-communication-characterization/run-multi-rx.sh`:
  1–4 one-hop senders × R1/R2/R4, full payload verification and exact flit
  counts, with traces checking simultaneous local outputs.
- `src/sst/tests/scratchpad_observer_test.cpp`: concurrent RX writes to
  separate banks versus the same bank.
- `src/sst/tests/rx_controller_regression.sh`: software-owned payloads,
  receive ordering and blocked-head ownership. Set
  `MITTENS_TEST_RX_STREAMS=4` to test the multi-RX configuration.

Local ejection activity is distinct from DMA activity. Resource stall sums
are not elapsed runtime; unavailable RX counters must remain unavailable.
