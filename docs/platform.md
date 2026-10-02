# Guest programming interface

Guests execute RV64/RVV code from local scratchpad starting at `0x90000000`.
The ELF, constants, data and stack must fit the configured SPM. Use the
[parameterized linker script](../src/platform/startup/scratchpad.ld) and match
its capacity to the simulation. Test guest startup is in
[`src/tests/riscv-qemu/start.S`](../src/tests/riscv-qemu/start.S).

Ordinary scalar and RVV loads/stores access local SPM through separate
load/store queues. The scalar queue defaults to eight entries;
`scalar_load_store_queue_depth=0` selects blocking scalar accesses. Unsupported
queued accesses drain older work and use the blocking path. Instruction-cache
fills consume SPM service. `fence.i` makes modified executable bytes visible.

`mvm.vset`, `mvm.vl` and `mvm.vs` move weights, inputs and outputs through
vector registers. `mvm` launches computation. See the
[complete instruction contract](../src/components/riscv-qemu/vector-analog.md).
Memory-address analog transfer instructions are retired and trap.

CPUs do not read another tile's scratchpad. The NIU transports payloads between
tiles with receiver storage reservations. The [guest network
interface](../src/components/mordred/network-instructions.md) provides `net.send`,
`net.recv`, `net.info`, `net.release`, and `net.wait`, with destination tile IDs for sends,
source-agnostic receive, and nonblocking receive/wait variants. Deployment data
reserves receive slots per transfer and carries compiler transfer/invocation IDs. Payloads pass through timed SPM service;
registers carry descriptors' addresses and completion tokens. Supported compiled
Sculptor deployment is currently [one tile](../src/tests/sculptor/README.md);
its runtime integration with this interface is separate work.
