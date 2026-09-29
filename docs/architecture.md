# Tile architecture

![Tile modules](diagrams/single-tile.svg)

A tile comprises a QEMU-backed RISC-V CPU, shared banked SPM, an analog
accelerator, a router-facing NIU, and a Mordred NIC/router. CPU and NIU requests
share actual SPM banks, ports and channels. Bank access lists select permitted
physical addresses. Code, data and stack reside in local SPM.

Instruction fetches use the instruction cache. Scalar memory operations are
blocking; eligible vector memory operations use the finite vector LSQ. The
analog command queue preserves dependencies on the same vector registers.
Accelerator weights, inputs and outputs travel only through the vector-register
interface. Each analog array can overlap its computation with input preparation
and output handling when software schedules independent work.

Network payload follows SPM → NIU → NIC → router → neighboring routers → NIC →
NIU → destination SPM. Router credits protect next-hop buffers; separate NIU
storage credits reserve receive capacity through destination bank completion.
Intermediate CPUs do not forward traffic. Current transfer initiation belongs
to an external test/study controller; a guest command interface is pending.

The SPM bus routes requests on its clock; it is not an independently bandwidth-
limited crossbar. Bank/channel scheduling supplies the local service constraints.
The diagram's internal blocks describe modeled functions, not separate SST
components in every case.

Implementation contracts: [CPU](../src/components/riscv-qemu/README.md),
[SPM](../src/components/scratchpad/README.md),
[accelerator](../src/components/analog-arrays/README.md),
[NIU](../src/components/mordred/spm-interface.md),
[mesh](../src/components/mordred/README.md).
