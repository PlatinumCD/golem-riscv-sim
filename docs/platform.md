# Guest programming interface

Guests execute RV64/RVV code from local scratchpad starting at `0x90000000`.
The ELF, constants, data and stack must fit the configured SPM. Use the
[parameterized linker script](../src/platform/startup/scratchpad.ld) and match
its capacity to the simulation. Test guest startup is in
[`src/tests/riscv-qemu/start.S`](../src/tests/riscv-qemu/start.S).

Ordinary scalar and RVV loads/stores access local SPM. Vector memory can use
the LSQ; scalar accesses retain blocking completion. Instruction-cache fills
consume SPM service. `fence.i` makes modified executable bytes visible.

`mvm.vset`, `mvm.vl` and `mvm.vs` move weights, inputs and outputs through
vector registers. `mvm` launches computation. See the
[complete instruction contract](../src/components/riscv-qemu/vector-analog.md).
Memory-address analog transfer instructions are retired and trap.

CPUs do not read another tile's scratchpad. The NIU transports payloads between
tiles with receiver storage reservations and destination arrival notifications.
A guest send/receive command interface is not yet implemented; current network
tests submit requests through an SST controller. Supported compiled Sculptor
deployment is currently [one tile](../src/tests/sculptor/README.md).
