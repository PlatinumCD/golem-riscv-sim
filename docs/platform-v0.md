# Golem Tile Platform v0

This document defines the software-visible address map for version 0 of a
single bare-metal Golem tile. Each tile has one RISC-V hart and its own private
address space.

## Address map

| Start | End | Size | Device or region | v0 use |
| ---: | ---: | ---: | --- | --- |
| `0x00100000` | `0x00100fff` | 4 KiB | QEMU test/exit device | Simulation exit |
| `0x02000000` | `0x0200ffff` | 64 KiB | RISC-V timer (CLINT) | Reserved; unused initially |
| `0x0c000000` | `0x0fffffff` | 64 MiB | RISC-V interrupt controller (PLIC) | Reserved; unused initially |
| `0x10000000` | `0x10000fff` | 4 KiB | UART | Debug and console output |
| `0x10010000` | `0x10010fff` | 4 KiB | SST mesh NIC | Mesh communication |
| `0x80000000` | `0x80ffffff` | 16 MiB | Tile-private RAM | Bare-metal ELF, static data, heap, and stack |

The bare-metal ELF is loaded at `0x80000000`. The internal layout of tile RAM,
including the heap and stack boundaries, will be defined by the linker script.

Addresses not listed above are not part of the Golem Tile Platform v0 software
ABI. QEMU may use other regions internally, but tile software must not depend
on them.

## Simulation architecture

Platform v0 uses one QEMU system-emulation process for each tile and one SST
`mittens.tile` component to own that process. QEMU executes the tile's RISC-V
instructions and services its local devices. SST owns simulated time, the mesh
topology, packet routing, and communication latency.

Each QEMU tile has:

- one RISC-V hart;
- 16 MiB of private RAM;
- one bare-metal ELF image loaded at `0x80000000`;
- the UART and simulation-exit devices listed in the address map; and
- one mesh NIC at `0x10010000`.

The QEMU processes do not share guest RAM. Data moves between tiles only as
packets through their mesh NICs.

### Tile lifecycle

SST is the lifecycle authority for every tile. During setup, `mittens.tile`
starts its QEMU child with the configured ELF and prevents SST from ending
while that child is running. The component monitors and reaps the child. A
normal QEMU exit releases SST's end-of-simulation hold; a signal or nonzero
exit is a simulation error. During normal or emergency SST shutdown, the
component terminates and reaps any QEMU child that is still running.

The initial implementation launches QEMU approximately as follows:

```text
qemu-system-riscv64 \
  -machine virt -smp 1 -m 16M \
  -bios none -kernel <tile.elf> \
  -display none -monitor none -serial stdio -no-reboot
```

The SST component has a linear `tile_id` used to identify its endpoint in the
mesh. Platform v0 does not yet define how software reads its own tile ID; a
future platform revision or a reserved NIC register may expose it if needed.

### Mesh data path

The intended packet path is:

```text
bare-metal tile program
        | mesh-NIC MMIO
QEMU mittens-nic device
        | shared-memory SPSC queues
SST mittens.tile
        | SST::Interfaces::SimpleNetwork
SST Merlin mesh
```

The QEMU NIC device converts guest MMIO operations into bridge operations.
Each tile owns one versioned host shared-memory region containing bounded
transmit and receive SPSC queues. The QEMU process produces the transmit queue
and consumes the receive queue; `mittens.tile` performs the opposite roles and
exchanges packets through `SST::Interfaces::SimpleNetwork`. This transport is
an implementation detail and does not change the four-register guest ABI
below.

Managed QEMU launch, monitoring, and cleanup; the QEMU NIC device; the
shared-memory bridge; and the SST SimpleNetwork/Merlin connection are
implemented and tested. The initial proof sends the float32 bit pattern for
`3.25` from tile 0 to tile 1 and returns an acknowledgment through the same
path. The bridge is currently polled. Host event notification and synchronization
between QEMU execution and SST simulated time remain future work.

## SST mesh NIC

The mesh NIC is a polling, programmed-I/O device. Each packet carries one
32-bit destination tile ID and one 32-bit payload. The payload is normally the
raw IEEE-754 representation of a 32-bit float; the NIC transports its bits
without interpreting or converting them.

All NIC registers are little-endian, 32-bit registers and require aligned
32-bit accesses. Register offsets are relative to the NIC base address
`0x10010000`.

| Offset | Register | Access | Meaning |
| ---: | --- | :---: | --- |
| `0x00` | `STATUS` | RO | Transmit and receive readiness |
| `0x04` | `TX_DESTINATION` | WO | Destination tile ID |
| `0x08` | `TX_DATA` | WO | Write one payload to transmit a packet |
| `0x0c` | `RX_DATA` | RO | Read and consume one received payload |

`STATUS` has the following bit assignments:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `TX_READY` | The NIC can accept a write to `TX_DATA` |
| 1 | `RX_VALID` | `RX_DATA` contains a payload that can be consumed |
| 2-31 | Reserved | Read as zero |

### Transmit behavior

Software waits for `TX_READY`, writes `TX_DESTINATION`, executes a RISC-V I/O
fence, and writes the raw 32-bit payload to `TX_DATA`. The `TX_DATA` write is
the transmit doorbell: it atomically forms and submits this packet:

```text
{ destination: TX_DESTINATION, payload: TX_DATA }
```

`TX_DESTINATION` remains latched until software changes it, allowing a tile
with a fixed successor to write its destination once during initialization.

### Receive behavior

Software waits for `RX_VALID` and reads `RX_DATA`. The read returns the oldest
waiting 32-bit payload and consumes that packet. If another packet is queued,
it becomes visible through `RX_DATA` and `RX_VALID` remains set.

The sender is implicit in the SST endpoint that injected the packet and is not
exposed to tile software. Each tile image contains its own computation routine,
so packets do not carry a routine identifier.

### Flow control and errors

SST provides backpressure and does not drop packets. Packets from a given
sender are delivered in order. Writing `TX_DATA` while `TX_READY` is clear,
reading `RX_DATA` while `RX_VALID` is clear, or using an invalid destination is
a platform protocol error. The simulator must stop with a diagnostic when it
detects one of these errors.

The following sequence is required for transmission:

```text
wait for TX_READY
write TX_DESTINATION
fence iorw, iorw
write TX_DATA
```

The following sequence is required for reception:

```text
wait for RX_VALID
read RX_DATA
```

## Stability

The base addresses and region sizes in this table are locked for Platform v0.
The four mesh NIC registers, their access behavior, and their status bits are
also locked for Platform v0. The remainder of the NIC's 4 KiB region is
reserved and reads as zero; writes to it are ignored.
