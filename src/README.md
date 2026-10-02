# Scratchpad, RISC-V, analog arrays and mesh network

`src` is the maintained simulator implementation. The `source_new` compatibility
symlink resolves here. It contains independent SST components for a banked scratchpad,
RV64/RVV execution, float32 analog arrays, and a Mordred mesh network. The
single-tile data path is:

```text
Banked SPM ⇄ CPU / RVV vector registers ⇄ Analog arrays
              instruction execution      weights, inputs, outputs
```

The arrays expose only a command/payload port. They have no StandardMem
interface, memory addresses, direct scratchpad connection, or legacy analog
memory bridge. To move array data to or from SPM, software uses ordinary RVV
memory instructions and `mvm.vset`, `mvm.vl`, or `mvm.vs`.

- [Scratchpad](components/scratchpad/README.md): SST functional storage with
  bank/channel timing and completed writes.
- [RISC-V QEMU](components/riscv-qemu/README.md): RV64/RVV in its own component
  directory, with an instruction cache, shared SPM bytes, and timed data accesses.
- [Analog arrays](components/analog-arrays/README.md): resident weights, input
  and output buffers, numerical MVM, and timed vector-register transfers.
- [Analog command queue](components/riscv-qemu/analog-command-queue.md): bounded
  asynchronous register transfers with shared RVV/LSQ dependency tracking.
- [Mordred mesh](components/mordred/README.md): pinned SST network components
  with a tested 2×2 mesh and payload-checking endpoints.
- [Mesh-connected tiles](components/mordred/spm-interface.md): complete RISC-V/SPM/array
  tiles with explicit CPU/router access to physical scratchpad banks.
- [DRAM Tile](components/dram_tile/README.md): a control CPU and timed DRAM
  interface that dispatches weights to compute tiles over the same mesh.
- [SST threading](../docs/threading.md): run a mesh across host workers while
  keeping each complete tile together and preserving simulated timing.
- [Instruction contract](components/riscv-qemu/vector-analog.md): assembly,
  chunk offsets/counts, compiler support, errors and timing.

## Small, Medium and Large profiles

[`tile_profiles.json`](tile_profiles.json) contains the current per-tile presets:
Small **1 MiB / 2 banks / VLEN 128**, Medium **1.5 MiB / 4 banks / VLEN 256**,
and Large **2 MiB / 8 banks / VLEN 512**. Every bank is shared by CPU and NIU.
Pass the selected entry's four parameter groups to `connect_riscv_mesh` along
with the guest ELFs, backing directory and deployment transfers. Array dimensions
and deployment storage remain workload choices; unspecified settings use the
component defaults. NIUs split whole messages up to `max_request_bytes` (256 B).

These capacities include guest code, stack, payloads and receive slots. The
32×32 network/MVM correctness fixtures fit the Small profile. A dense 1024×1024
FP32 weight image needs 4 MiB before other allocations, so the old 32 MiB study
layout must be replaced with a bounded working set before reusing that workload.
Historical bandwidth measurements with 32 MiB are not measurements of these presets.

## Build and validate

From the repository root:

```bash
python3 -B src/components/riscv-qemu/build_qemu.py
python3 -B src/build.py
python3 -B src/tests/test_configuration.py
python3 -B src/tests/mordred/test_configuration.py
python3 -B src/tests/network-instructions/test_configuration.py
python3 -B src/tests/mordred/run.py
python3 -B src/tests/run.py
python3 -B src/tests/riscv-qemu/run.py
python3 -B src/tests/vector-memory/run.py
python3 -B src/tests/load-store-queue/run.py
python3 -B src/tests/scalar-load-store-queue/run.py
python3 -B src/tests/analog-register-dependencies/run.py
python3 -B src/tests/instruction-cache/run.py
python3 -B src/tests/vector-analog/run.py
python3 -B src/tests/array-pipeline/run.py
python3 -B src/tests/single-tile-runtime/run.py
```

The builds use installed shared SST libraries and pinned QEMU sources, producing
`build/src/components/libtilecomponents.so`, `build/src/components/libmordred.so`, and
`build/src/qemu/qemu-system-riscv64`. The old simulator is retired; see the [migration record](../docs/migration.md).
The standard build is `JOBS=8 bash bootstrap.sh build-hardware`, and the complete
correctness entry point is `bash tests/run-all.sh --suite hardware`.

The component suite tests scratchpad service and independent array payload
commands. The CPU suite checks scalar/RVV memory operations, sharing and
ordering. Vector-analog tests run LLVM-compiled guests and inspect both
numerical results and the actual SST graph: arrays connect only to the CPU's
command port. Slower SPM service changes CPU runtime without changing array
command latency. Retired memory-based analog instructions must trap before
accessing their address operands. Results are stored under `tests/results/`.
The instruction-cache suite checks fills, hits, replacement, boundary cases,
and code visibility after `fence.i`.
The vector-memory suite checks grouped RVV accesses, real bank parallelism,
mask/tail preservation, restart/fault behavior and shared-SPM ordering.

The [single-tile runtime benchmark](tests/single-tile-runtime/README.md) compares
one 32×32 or 64×64 array at VLEN 256 and 512 with a matrix matching the array's
size. It reports cold programming plus execution and repeated MVMs with resident
weights, measured in SST cycles with all outputs checked. Matrix work differs
between the two array sizes.

Cycle profiling is optional and disabled by default. See
[profiling controls and trace formats](../docs/profiling.md) for the command-line
flag, Python configuration and maintained on/off regression.

## Sculptor compiler integration

Sculptor now lowers analog setup/input/output operations to RVV chunk loops
using `mvm.vset`, `mvm.vl`, and `mvm.vs`. Its normal tensor, partition, ownership,
and LLVM passes produce the computation; a bounded local payload runtime
supplies inputs and captures outputs on one tile.

```sh
GOLEM_BUILD_SCOPE=shared GOLEM_SCULPTOR_SOURCE="$PWD/third_party/sculptor-mlir" \
  bash build-scripts/build-sculptor-mlir.sh
python3 -B src/tests/sculptor/run.py
```

The [integration suite](tests/sculptor/README.md) covers linear/ReLU,
linear/sigmoid, four packed arrays, convolution, padding, RVV tails, and repeated
inputs with resident weights. The current supported deployment is one tile;
this runtime uses bounded local SPM copies. The hardware's
[guest network interface](components/mordred/network-instructions.md) is available
for multi-tile runtime integration.
Use VLEN >= 256 for digitally vectorized `golem-analog` programs.

## Composition

```python
from configuration import connect_riscv_arrays

cpu, arrays, scratchpad = connect_riscv_arrays(
    sst, {"array_rows": 32, "array_cols": 32, "riscv_vector_length_bits": 512},
    elf="/absolute/path/to/guest.elf",
    memory_file="/absolute/path/to/run/scratchpad.bin",
)
```

`connect_riscv()` creates CPU plus SPM. `connect_arrays()` and
`connect_scratchpad()` instantiate the individual components for fixtures.
`connect()` returns independent arrays and scratchpad, with only its explicit
StandardMem clients attached to SPM. Test command drivers represent a source
of vector payloads; they do not create a direct array memory connection.

The default composition contains one physical analog array, addressed as array 0.
The CPU helper zeroes the run-specific SPM backing file and requires an ELF
linked into local SPM starting at `0x90000000`. The example sets VLEN to 512
bits for both components, giving an array link width of 64 bytes/cycle.
See the CPU README for its instruction-budget, issue-width and other RVV settings.

`components.mordred.tiles.connect_riscv_mesh()` creates a complete tile at each
mesh router, with independent backing files and component names. Its default
SPM has four physical banks: `cpu_spm_banks=[0,1,2,3]` and
`router_spm_banks=[2,3]`. The router interface issues timed StandardMem requests
to those actual banks; it shares their existing ports and channels with the
CPU. A request touching a bank outside its list is rejected before functional
memory changes. Guest CPUs use [network instructions](components/mordred/network-instructions.md)
for whole-message sends and receive ownership, with no guest polling engine or
CPU payload copying. See the [router-facing SPM interface](components/mordred/spm-interface.md)
for the underlying packet transport and bank service.

Instruction fetches use an 8 KiB, two-way cache with 64-byte lines and one-cycle
hits by default. Misses fill lines through the banked SPM. `fence.i` invalidates
cached instruction lines. CPU `instruction_cache_*` options configure the cache;
`instruction_cache_enabled=False` provides a direct-fetch control. Data loads
and stores remain uncached. Contiguous active RVV elements use timed beats of
at most VLEN/8 bytes, split into requests at SPM ordering-line boundaries.
The dedicated [scalar load/store queue](components/riscv-qemu/scalar-load-store-queue.md)
defaults to eight entries on all tile profiles. Set
`cpu_parameters={"scalar_load_store_queue_depth": 0}` for blocking scalar accesses,
or select 1–64 entries. It shares the existing SPM banks and preserves register
dependencies, byte-range ordering, and precise fault boundaries.

`cpu_parameters={"load_store_queue_depth": 8}` enables multiple outstanding
eligible vector transfers, with captured stores, load dependency tracking and
ordered completion. Depth 1 is the unchanged blocking baseline; valid depths
are 1–64. See the [load/store queue contract](components/riscv-qemu/load-store-queue.md)
for supported accesses and conservative fallback behavior. Eligible whole-register
and integer-LMUL 2/4/8 transfers use the same queue as LMUL1, split into
VLEN-sized beats with dependencies across every register in the group.
Legal analog instructions use those register dependencies too: unrelated
queued memory can progress while `mvm.vset`, `mvm.vl`, or `mvm.vs` transfers
register data. The analog command queue defaults to four entries, allowing
multiple vector analog commands and independent CPU work after safe admission.
Set `cpu_parameters={"analog_command_queue_depth": 0}` to disable this queue.
The [queue contract](components/riscv-qemu/analog-command-queue.md) specifies
source capture, pending destinations, finite register-port bandwidth, and
precise exceptions. This setting is independent of LSQ depth and array pipelining.
The [directed regression suite](tests/analog-register-dependencies/README.md)
checks this overlap and preserves register hazards, tails and precise traps.

## Architecture parameters

The CPU, SPM and arrays use a fixed 1 GHz clock. Link widths below are bytes
per cycle. The independent mesh has its own clock and flit-width parameters.

| Parameter | Default | Meaning |
|---|---:|---|
| `cost_per_array_program_cycles` | 0 | Programming latency after each nonempty weight chunk arrives |
| `cost_per_mvm_cycles` | 100 | Execution latency once weights and input are initialized |
| `arrays_per_tile` | 1 | Independent analog arrays per tile |
| `array_rows` | 32 | Output elements per array |
| `array_cols` | 32 | Input elements per array |
| `riscv_vector_length_bits` | 256 | Shared VLEN in bits; 128, 256, 512 or 1024 |
| `array_link_width` | 32 | Derived bytes/cycle: VLEN / 8; not an independent setting |
| `array_inflight_bytes` | 64 | Transfer bytes buffered on the array link per array |
| `array_link_duplex` | shared | One bidirectional link, or independent input/output directions |
| `array_pipeline_enabled` | true | Overlap one array computation with input/output work; requires a pipelined guest schedule |
| `spm_capacity_bytes` | 2097152 | Scratchpad storage capacity; CPU composition supports up to 32 MiB |
| `spm_banks` | 8 | Interleaved SRAM banks |
| `cpu_spm_banks` | all physical banks | Bank IDs accessible to CPU instruction/data requests; resolves from `None` |
| `router_spm_banks` | [] | Bank IDs accessible to a named router interface; resolves from `None` in standalone configurations |
| `spm_bank_width` | 4 | Bytes per bank port per cycle and address stripe size |
| `spm_read_ports_per_bank` | 1 | Read ports per bank |
| `spm_write_ports_per_bank` | 1 | Write ports per bank |
| `spm_channels` | 2 | Memory channels shared by CPU and explicit memory clients |
| `spm_channel_width` | 32 | Bytes served by each memory channel per cycle |
| `spm_request_bytes` | 32 | Maximum SPM request bytes and controller ordering-line size |

The standalone default SPM has eight 32-bit (4-byte) banks. An aligned 32-byte request
uses all eight banks in one service cycle. The banks provide 32 bytes/cycle
for reads and 32 bytes/cycle for writes through separate bank ports, subject
to the shared channels. An aligned full 256-bit unit-stride vector load issues
one 32-byte request; a 512-bit load issues two such requests. Bank service and
transport both contribute to completion latency. Masks, tails and boundaries
can shorten beats; unsupported or unsafe forms retain element accesses.

Bank lists contain unique integers in `range(spm_banks)`. They select physical
addresses using `bank = floor(address / spm_bank_width) % spm_banks`; they do
not allocate separate service quotas or remap addresses. Overlapping lists
share the same banks and arbitration. A real CPU requires a nonempty CPU list,
and a tile mesh requires a nonempty router list. The mesh helper defaults its
bank count to four and its router list to the highest two bank IDs (or the
single bank when only one exists). Explicit lists survive resolved snapshots.

Changing `riscv_vector_length_bits` changes the CPU register size and array-link
width together. Resolved configurations report `array_link_width`; supplying a
conflicting value is an error. The older CPU-parameter spelling of VLEN is
accepted when it agrees with any explicitly supplied architecture VLEN.
LMUL changes the number of registers in a group, without multiplying link width.

`array_pipeline_enabled=True` is the default in both the CPU and arrays.
Set it explicitly to `False` for a blocking execution control. Each array has one compute engine,
an input snapshot, and two result slots. `mvm` returns after validated compute
start; `mvm.vs` reads the oldest result and consumes it after all output
elements have been read. Software must drain that result before submitting a
third job. Fences, task markers, and guest exit wait for started computations.
RVV memory grouping works with either array execution mode. Use
`python3 -B src/tests/single-tile-runtime/run.py` to use
both this mode and a matching guest schedule; pass `--no-pipeline` for the blocking
control. Enabling the hardware mode does not rewrite a serial guest schedule.
See the [instruction contract](components/riscv-qemu/vector-analog.md)
for result consumption and programming restrictions.

Array dimensions and transfer buffering are independent of SPM capacity.
Array transfers consume their own link bandwidth and configured device delays.
They never consume scratchpad banks or memory channels. CPU loads/stores used
to prepare or save vector registers consume ordinary SPM service.

Fixed resources include one-cycle bank latency, 64 backend request slots,
four command slots per array, one-cycle array-link transit, and 1 ns SST
transport links. Memory fixtures can override backend admission capacity with
`experimental={"queue_entries": ...}`. The numerical MVM uses ideal arithmetic;
this is not an analog circuit, noise, or energy simulation.

The isolated QEMU build supports `mvm` and the three vector transfers. It rejects
memory-based `mvm.set/l/s/mv` instructions and contains no legacy analog device.
Removed direct-memory settings such as `spm_to_array_link_width` and
`spm_channels_shared` are rejected by the configuration API.
