# Tile parameters

Generated from [tileParameters.h](../src/sst/configuration/tileParameters.h).
Values below are defaults; a simulation may override them.

Set these keys on `mittens.tile` in the SST configuration. Invalid combinations
are rejected by `TileConfiguration::validate`.

Categories: **hardware** describes modeled resources, **execution** controls
host execution and replay, **workload** selects guest inputs, and
**measurement** controls output. `scratchpad_access_width_bits` is currently
exported as execution metadata but sets the modeled SPM port width.

Regenerate with `python3 tools/hardware/parameter-reference.py`;
use `--check` to detect stale documentation.

## Tile identity and mesh topology

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `tile_id` | `std::uint32_t` | `0` | hardware | Linear mesh tile identifier |
| `network_size` | `std::uint32_t` | `0` | hardware | Number of valid destination tile IDs |
| `mesh_width` | `std::uint32_t` | `0` | hardware | Physical mesh width used for hop accounting; zero means unspecified |
| `mesh_height` | `std::uint32_t` | `0` | hardware | Physical mesh height used for hop accounting; zero means unspecified |
| `mesh_link_clock` | `std::string` | `1GHz` | hardware | Clock defining one physical mesh transfer cycle |
| `mesh_link_width_bits` | `std::uint32_t` | `32` | hardware | Physical mesh link width in bits per transfer cycle |
| `network_packet_words` | `std::uint32_t` | `16` | hardware | Maximum 32-bit words in one SST network request. This value must not exceed the endpoint buffer capacity |

## Guest program

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `qemu_path` | `std::string` | `qemu-system-riscv64` | execution | Path to the QEMU system emulator |
| `elf` | `std::string` | `` | workload | Bare-metal ELF image loaded by QEMU |

## Executable scratchpad and its instruction cache. No data cache is added

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `instruction_cache_bytes` | `std::uint32_t` | `8192` | hardware | Instruction cache capacity in bytes, backed by scratchpad |
| `instruction_cache_line_bytes` | `std::uint32_t` | `64` | hardware | Instruction cache line size in bytes |
| `instruction_cache_ways` | `std::uint32_t` | `2` | hardware | Instruction cache associativity |
| `instruction_cache_hit_cycles` | `std::uint64_t` | `1` | hardware | Instruction cache lookup latency in CPU cycles |

## Private noncoherent scratchpad

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `scratchpad_bytes` | `std::uint64_t` | `262144` | hardware | Private scratchpad capacity in bytes |
| `scratchpad_banks` | `std::uint32_t` | `8` | hardware | Number of scratchpad banks |
| `scratchpad_read_ports` | `std::uint32_t` | `1` | hardware | Read ports per scratchpad bank |
| `scratchpad_write_ports` | `std::uint32_t` | `1` | hardware | Write ports per scratchpad bank |
| `scratchpad_access_width_bits` | `std::uint32_t` | `256` | execution | Scratchpad port width in bits |
| `scratchpad_latency_cycles` | `std::uint64_t` | `1` | hardware | Scratchpad access latency in cycles |
| `scratchpad_dma_bytes_per_cycle` | `std::uint32_t` | `32` | hardware | Scratchpad DMA width in bytes per cycle |
| `scratchpad_dma_setup_cycles` | `std::uint64_t` | `8` | hardware | Scratchpad DMA setup latency in cycles |

## Deployment-wide memory and epoch controls

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `global_ram_bytes` | `std::uint64_t` | `34359738368` | hardware | Deployment-wide sparse global RAM capacity |
| `epoch_barrier_epochs` | `std::uint32_t` | `0` | hardware | Number of modeled deployment epochs, including boot epoch zero; zero disables the epoch barrier |

## CPU clock, issue, and QEMU scheduling controls

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `launch_mode` | `std::string` | `disabled` | workload | QEMU launch mode: disabled or managed |
| `cpu_clock` | `std::string` | `1GHz` | hardware | Clock defining the synchronized CPU issue cycle |
| `cpu_issue_width` | `std::uint32_t` | `1` | hardware | Scalar issue width; executable-SPM fetch accounting currently requires 1 |
| `sync_instruction_quantum` | `std::uint64_t` | `1000` | execution | Maximum instructions SST grants QEMU at once |
| `qemu_capture_spin_us` | `std::uint32_t` | `0` | execution | Host-only busy-poll interval before an SST-to-QEMU capture falls back to futex sleep |

## RISC-V Vector Extension

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `riscv_vector_enabled` | `bool` | `true` | hardware | Enable the standard RISC-V V extension in QEMU |
| `riscv_vector_length_bits` | `std::uint32_t` | `256` | hardware | QEMU RISC-V vector register length (VLEN), a power of two from 128 through 1024 bits |
| `riscv_vector_element_bits` | `std::uint32_t` | `64` | hardware | QEMU maximum RISC-V vector element width (ELEN), a power of two from 8 through 64 bits |

## Receive and transmit DMA frontends

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `rx_dma_clock` | `std::string` | `1GHz` | hardware | Clock for the tile-local NIC-to-scratchpad receive DMA engine |
| `rx_dma_width_bits` | `std::uint32_t` | `256` | hardware | Receive DMA transfer width; positive multiple of 32 bits |
| `rx_dma_setup_cycles` | `std::uint64_t` | `8` | hardware | One-time setup cycles charged per receive descriptor |
| `rx_dma_queue_depth` | `std::uint32_t` | `4` | hardware | Finite incoming burst queue depth, from 1 through 4 |
| `rx_dma_streams` | `std::uint32_t` | `1` | hardware | Independent RX DMA lanes: 1, 2, 4 |
| `rx_dma_streaming` | `bool` | `false` | hardware | Release arrived DMA-owned frame fragments before the whole frame arrives |
| `tx_dma_fifo_bytes` | `std::uint32_t` | `128` | hardware | Per-lane source-SPM-to-NIC FIFO bytes, at least one scratchpad DMA beat |
| `tx_dma_streams` | `std::uint32_t` | `1` | hardware | Independent TX DMA and local injection lanes: 1, 2, or 4 |

## Analog accelerator

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `analog_array_count` | `std::uint32_t` | `0` | hardware | Simulation-wide number of analog arrays instantiated on every tile; zero disables analog |
| `analog_array_rows` | `std::uint32_t` | `100` | hardware | Simulation-wide row count shared by every analog array |
| `analog_array_columns` | `std::uint32_t` | `100` | hardware | Simulation-wide column count shared by every analog array |
| `analog_backend` | `std::string` | `native` | hardware | Analog numerical backend: native or crosssim |
| `crosssim_config` | `std::string` | `` | workload | Optional CrossSim JSON parameter file used by this tile's independent backend |
| `analog_link_clock` | `std::string` | `1GHz` | hardware | Clock for the tile-wide shared bidirectional 256-bit analog link |
| `analog_compute_latency_cycles` | `std::uint64_t` | `100` | hardware | Analog compute latency in analog-link cycles |

## Measurement and diagnostics

| Parameter | Type | Default | Category | Meaning |
|---|---|---|---|---|
| `profile_mode` | `std::string` | `off` | measurement | Performance profiling mode: off, summary, or trace |
| `profile_output_directory` | `std::string` | `` | measurement | Directory for per-tile performance profile files |
| `progress_snapshot_interval_ms` | `std::uint64_t` | `10000` | measurement | Wall-time interval for bounded progress snapshots; zero disables periodic snapshots |
| `progress_watchdog_ms` | `std::uint64_t` | `60000` | measurement | Wall-time interval without deployment-wide architectural progress on this SST rank before dumping diagnostics and failing; zero disables the watchdog |
| `task_trace_directory` | `std::string` | `` | measurement | Optional directory for per-tile task trace CSV files |
| `serial_output_directory` | `std::string` | `` | measurement | Optional directory for isolated per-tile QEMU UART logs |
| `verbose` | `int` | `0` | measurement | Mittens diagnostic verbosity |
