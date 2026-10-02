# DRAM Tile

A DRAM Tile supplies weights to compute tiles over the existing Mordred mesh.
It combines a RISC-V control CPU, local control SPM, a DRAM-backed NIU
(`tilecomponents.DramTile`), an SST memory controller, and `memHierarchy.timingDRAM`.
The mesh supplies its router. A DRAM Tile has no analog array.

The control CPU issues `net.send(destination, descriptor)` from local SPM.
The NIU captures that descriptor, reads its payload through timed DRAM service,
and sends ordinary whole-message packets. Destination tiles receive into their
reserved SPM slots. Their CPU/RVV instructions can program an analog array
directly from those slots before releasing them. Payloads do not pass through
the source CPU's registers or a source SPM staging buffer.

## Use in a mesh

```python
from components.mordred.tiles import connect_riscv_mesh

network = connect_riscv_mesh(
    sst, parameters, elfs=tile_elfs, qemu=qemu,
    memory_directory=run_directory,
    mesh_parameters=dict(x_dim=2, y_dim=2),
    network_transfers=deployed_transfers,
    dram_tiles={
        0: dict(image="weights.bin", capacity_bytes=64*1024*1024,
                channels=2, banks_per_rank=8, queue_depth=32),
    },
)
```

Keys in `dram_tiles` are row-major tile IDs. Other positions remain compute
tiles. Every position still needs an ELF; the DRAM tile's ELF is its control
program. An empty mapping preserves the existing compute-only composition.

The raw little-endian image must be exactly `capacity_bytes` long; sparse files
are supported. Byte zero corresponds to `base` in send descriptors. The image
initializes DRAM before guest execution; every subsequent weight fetch has
simulated DRAM latency. A separate `tileN-dram.bin` backing output preserves
the input image. Input/output aliases are rejected before file mutation.

The returned DRAM tile dictionary has `arrays=None`, plus `dram_controller`,
`dram_backend`, `dram_parameters` and `dram_image`. Its `scratchpad` is the
control SPM, and `router_spm` is the network endpoint.

## Memory and message contract

- Descriptor pointers still identify local control SPM. For a DRAM Tile,
  `source_address` identifies its configured DRAM region. Compute-tile payload
  addresses continue to identify local SPM. The 32-byte descriptor, instruction
  encodings, transfer IDs and invocation IDs are unchanged.
- The DRAM address region is available to the NIU for reads. It is not mapped
  for CPU loads/stores. This first component supplies preloaded weights;
  runtime DRAM writes and compiler-generated weight placement are separate work.
- Incoming messages still use deployed local SPM slots. `net.recv`, `net.info`,
  `net.release` and `net.wait` retain the existing ownership/credit rules.
  `net.wait` does not wait for the destination to consume its weights.
- The common NIU provides bounded commands, tickets, payload buffers, packet
  credits and application-slot credits. `memory_queue_depth` caps outstanding
  memory fragments across descriptor reads, DRAM reads and incoming SPM writes.
  The DRAM backend has an additional finite transaction queue per channel.
- DRAM responses assemble packet data within the existing NIU request window.
  Network/receive-slot backpressure limits further DRAM reads. Other transfers
  continue within the available command, memory and packet capacity.

## DRAM settings

All settings below belong inside one `dram_tiles` entry. `image` is required.
Existing CPU/SPM/router settings retain their existing configuration locations.
The default DRAM has two channels, one rank per channel and eight banks per
rank (sixteen banks total), sharing 64 MiB of capacity. Control and compute
SPMs default to one write port per bank (`spm_write_ports_per_bank=1`);
this is an SPM setting, separate from the DRAM channels.

| Parameter | Default | Meaning |
|---|---:|---|
| `capacity_bytes` | 67,108,864 | DRAM capacity and image size |
| `base` | `0x100000000` | Payload address base used in descriptors |
| `request_bytes` | 64 | Maximum read fragment and address boundary; power of two, at most 64 |
| `channels` | 2 | DRAM channels |
| `ranks_per_channel` | 1 | Ranks per channel |
| `banks_per_rank` | 8 | Banks per rank |
| `row_bytes` | 1024 | Power-of-two row size, at least `request_bytes` |
| `queue_depth` | 32 | Pending DRAM transactions per channel |
| `t_cas_cycles` | 12 | Column access latency |
| `t_rcd_cycles` | 12 | Row activation latency |
| `t_rp_cycles` | 12 | Precharge latency |
| `burst_cycles` | 4 | Data-bus occupancy per backend transaction |
| `clock` | `1GHz` | DRAM/controller clock; timing values above use this clock |

The backend uses FIFO transaction queues and an open-page policy. These are
starting model parameters, not calibration to a particular DRAM device.
`timingDRAM` accounts for channel data-bus use and bank/row timing; it is not a
complete modern DDR device model.

## Profiling and validation

`DRAM_TILE_STATS` reports payload bytes, read count, peak outstanding reads,
sum of request latencies and cycles with at least one DRAM read outstanding.
These latency/busy counts use the 1 GHz endpoint clock and include controller
queueing and interface latency; they are not DRAM bank utilization.
SPM byte counters exclude DRAM payload reads.

With test diagnostics or profiling enabled, `*-dram.csv` records each read's
issue and response. `TILE_CYCLE_PROFILE=1` also enables a DRAM pending-read
timeline using the existing optional profiling mechanism. Network message,
packet, credit, SPM, CPU and array traces remain available.

```sh
python3 -B src/tests/dram-tile/test_configuration.py
python3 -B src/tests/dram-tile/run.py --output /tmp/golem-dram-tile-check
```

The normal hardware suite includes this as `network/dram-tile`:
`bash tests/run-all.sh --case network/dram-tile`. Its host configuration checks
also run in `python3 -B tools/hardware/verify.py`.

The end-to-end test uses real QEMU control/compute guests in a 2x2 mesh. A DRAM
tile streams two different 32x32 matrices in 512-byte messages to tiles 1 and 3,
which program arrays and execute checked MVMs. It covers delayed single-slot
consumers, unaligned source reads, slower DRAM with a one-entry channel queue,
the default two channels and an explicit one-channel override, invalid source
regions, descriptor reuse, and identical timing with profiling on/off.
Both scalar and vector LSQs are enabled. It verifies source read bounds,
destination visibility/ownership, queue drainage and unchanged weight images.
