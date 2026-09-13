#pragma once

// Single source of truth for tile parameters.
//
// Each entry is:
//   X(type, C++ name, SST name, default, group, description, display default)
//
// Categories: hardware = modeled resources; execution = host replay controls;
// workload = guest inputs; measurement = diagnostics and output.
// Both default fields must agree: default is C++, display default is SST text.
// See docs/parameters.md for a generated table of values and descriptions.
// MITTENS_TILE_PARAMETERS at the bottom expands every group in declaration order.

// Tile identity and mesh topology.
#define MITTENS_TILE_TOPOLOGY_PARAMETERS(X)                                                      \
    X(std::uint32_t, tileId,                                                                     \
      "tile_id", 0, "hardware",                                                                  \
      "Linear mesh tile identifier",                                                             \
      "0")                                                                                       \
    X(std::uint32_t, networkSize,                                                                \
      "network_size", 0, "hardware",                                                             \
      "Number of valid destination tile IDs",                                                    \
      "0")                                                                                       \
    X(std::uint32_t, meshWidth,                                                                  \
      "mesh_width", 0, "hardware",                                                               \
      "Physical mesh width used for hop accounting; zero means unspecified",                     \
      "0")                                                                                       \
    X(std::uint32_t, meshHeight,                                                                 \
      "mesh_height", 0, "hardware",                                                              \
      "Physical mesh height used for hop accounting; zero means unspecified",                    \
      "0")                                                                                       \
    X(std::string, meshLinkClock,                                                                \
      "mesh_link_clock", "1GHz", "hardware",                                                     \
      "Clock defining one physical mesh transfer cycle",                                         \
      "1GHz")                                                                                    \
    X(std::uint32_t, meshLinkWidthBits,                                                          \
      "mesh_link_width_bits", 32, "hardware",                                                    \
      "Physical mesh link width in bits per transfer cycle",                                     \
      "32")                                                                                      \
    X(std::uint32_t, networkPacketWords,                                                         \
      "network_packet_words", 16, "hardware",                                                    \
      "Maximum 32-bit words in one SST network request. This value must not exceed "             \
      "the endpoint buffer capacity",                                                            \
      "16")

// Guest program.
#define MITTENS_TILE_MEMORY_PARAMETERS(X)                                                        \
    X(std::string, qemuPath,                                                                     \
      "qemu_path", "qemu-system-riscv64", "execution",                                           \
      "Path to the QEMU system emulator",                                                        \
      "qemu-system-riscv64")                                                                     \
    X(std::string, elfPath,                                                                      \
      "elf", "", "workload",                                                                     \
      "Bare-metal ELF image loaded by QEMU",                                                     \
      "")


// Executable scratchpad and its instruction cache. No data cache is added.
#define MITTENS_TILE_INSTRUCTION_PARAMETERS(X)                                                    \
    X(std::uint32_t, instructionCacheBytes,                                                       \
      "instruction_cache_bytes", 8192, "hardware",                                                \
      "Instruction cache capacity in bytes, backed by scratchpad",                                \
      "8192")                                                                                     \
    X(std::uint32_t, instructionCacheLineBytes,                                                   \
      "instruction_cache_line_bytes", 64, "hardware",                                             \
      "Instruction cache line size in bytes",                                                     \
      "64")                                                                                       \
    X(std::uint32_t, instructionCacheWays,                                                        \
      "instruction_cache_ways", 2, "hardware",                                                    \
      "Instruction cache associativity",                                                          \
      "2")                                                                                        \
    X(std::uint64_t, instructionCacheHitCycles,                                                   \
      "instruction_cache_hit_cycles", 1, "hardware",                                              \
      "Instruction cache lookup latency in CPU cycles",                                           \
      "1")

// Private noncoherent scratchpad.
#define MITTENS_TILE_SCRATCHPAD_PARAMETERS(X)                                                    \
    X(std::uint64_t, scratchpadBytes,                                                            \
      "scratchpad_bytes", 256 * 1024, "hardware",                                                \
      "Private scratchpad capacity in bytes",                                                    \
      "262144")                                                                                  \
    X(std::uint32_t, scratchpadBanks,                                                            \
      "scratchpad_banks", 8, "hardware",                                                         \
      "Number of scratchpad banks",                                                              \
      "8")                                                                                       \
    X(std::uint32_t, scratchpadReadPorts,                                                        \
      "scratchpad_read_ports", 1, "hardware",                                                    \
      "Read ports per scratchpad bank",                                                          \
      "1")                                                                                       \
    X(std::uint32_t, scratchpadWritePorts,                                                       \
      "scratchpad_write_ports", 1, "hardware",                                                   \
      "Write ports per scratchpad bank",                                                         \
      "1")                                                                                       \
    X(std::uint32_t, scratchpadAccessWidthBits,                                                  \
      "scratchpad_access_width_bits", 256, "execution",                                          \
      "Scratchpad port width in bits",                                                           \
      "256")                                                                                     \
    X(std::uint64_t, scratchpadLatencyCycles,                                                    \
      "scratchpad_latency_cycles", 1, "hardware",                                                \
      "Scratchpad access latency in cycles",                                                     \
      "1")                                                                                       \
    X(std::uint32_t, scratchpadDMABytesPerCycle,                                                 \
      "scratchpad_dma_bytes_per_cycle", 32, "hardware",                                          \
      "Scratchpad DMA width in bytes per cycle",                                                 \
      "32")                                                                                      \
    X(std::uint64_t, scratchpadDMASetupCycles,                                                   \
      "scratchpad_dma_setup_cycles", 8, "hardware",                                              \
      "Scratchpad DMA setup latency in cycles",                                                  \
      "8")

// Deployment-wide memory and epoch controls.
#define MITTENS_TILE_GLOBAL_PARAMETERS(X)                                                        \
    X(std::uint64_t, globalRAMBytes,                                                             \
      "global_ram_bytes", UINT64_C(34359738368), "hardware",                                     \
      "Deployment-wide sparse global RAM capacity",                                              \
      "34359738368")                                                                             \
    X(std::uint32_t, epochBarrierEpochs,                                                         \
      "epoch_barrier_epochs", 0, "hardware",                                                     \
      "Number of modeled deployment epochs, including boot epoch zero; zero disables "           \
      "the epoch barrier",                                                                       \
      "0")

// CPU clock, issue, and QEMU scheduling controls.
#define MITTENS_TILE_CPU_PARAMETERS(X)                                                           \
    X(std::string, launchMode,                                                                   \
      "launch_mode", "disabled", "workload",                                                     \
      "QEMU launch mode: disabled or managed",                                                   \
      "disabled")                                                                                \
    X(std::string, cpuClock,                                                                     \
      "cpu_clock", "1GHz", "hardware",                                                           \
      "Clock defining the synchronized CPU issue cycle",                                         \
      "1GHz")                                                                                    \
    X(std::uint32_t, cpuIssueWidth,                                                              \
      "cpu_issue_width", 1, "hardware",                                                          \
      "Scalar issue width; executable-SPM fetch accounting currently requires 1",                                                                   \
      "1")                                                                                       \
    X(std::uint64_t, syncInstructionQuantum,                                                     \
      "sync_instruction_quantum", 1000, "execution",                                             \
      "Maximum instructions SST grants QEMU at once",                                            \
      "1000")                                                                                    \
    X(std::uint32_t, qemuCaptureSpinMicroseconds,                                                \
      "qemu_capture_spin_us", 0, "execution",                                                    \
      "Host-only busy-poll interval before an SST-to-QEMU capture falls back to futex"           \
      " sleep",                                                                                  \
      "0")

// RISC-V Vector Extension.
#define MITTENS_TILE_RVV_PARAMETERS(X)                                                           \
    X(bool, riscvVectorEnabled,                                                                  \
      "riscv_vector_enabled", true, "hardware",                                                  \
      "Enable the standard RISC-V V extension in QEMU",                                          \
      "true")                                                                                    \
    X(std::uint32_t, riscvVectorLengthBits,                                                      \
      "riscv_vector_length_bits", 256, "hardware",                                               \
      "QEMU RISC-V vector register length (VLEN), a power of two from 128 through "              \
      "1024 bits",                                                                               \
      "256")                                                                                     \
    X(std::uint32_t, riscvVectorElementBits,                                                     \
      "riscv_vector_element_bits", 64, "hardware",                                               \
      "QEMU maximum RISC-V vector element width (ELEN), a power of two from 8 through"           \
      " 64 bits",                                                                                \
      "64")

// Receive and transmit DMA frontends.
#define MITTENS_TILE_DMA_PARAMETERS(X)                                                           \
    X(std::string, receiveDMAClock,                                                              \
      "rx_dma_clock", "1GHz", "hardware",                                                        \
      "Clock for the tile-local NIC-to-scratchpad receive DMA engine",                           \
      "1GHz")                                                                                    \
    X(std::uint32_t, receiveDMAWidthBits,                                                        \
      "rx_dma_width_bits", 256, "hardware",                                                      \
      "Receive DMA transfer width; positive multiple of 32 bits",                                \
      "256")                                                                                     \
    X(std::uint64_t, receiveDMASetupCycles,                                                      \
      "rx_dma_setup_cycles", 8, "hardware",                                                      \
      "One-time setup cycles charged per receive descriptor",                                    \
      "8")                                                                                       \
    X(std::uint32_t, receiveDMAQueueDepth,                                                       \
      "rx_dma_queue_depth", 4, "hardware",                                                       \
      "Finite incoming burst queue depth, from 1 through 4",                                     \
      "4")                                                                                       \
    X(std::uint32_t, receiveDMAStreams,                                                          \
      "rx_dma_streams", 1, "hardware",                                                           \
      "Independent RX DMA lanes: 1, 2, 4",                                                       \
      "1")                                                                                       \
    X(bool, receiveDMAStreaming,                                                                 \
      "rx_dma_streaming", false, "hardware",                                                     \
      "Release arrived DMA-owned frame fragments before the whole frame arrives",                \
      "false")                                                                                   \
    X(std::uint32_t, transmitDMAFIFOBytes,                                                       \
      "tx_dma_fifo_bytes", 128, "hardware",                                                      \
      "Per-lane source-SPM-to-NIC FIFO bytes, at least one scratchpad DMA beat",                   \
      "128")                                                                                     \
    X(std::uint32_t, transmitDMAStreams,                                                         \
      "tx_dma_streams", 1, "hardware",                                                           \
      "Independent TX DMA and local injection lanes: 1, 2, or 4",                                \
      "1")

// Analog accelerator.
#define MITTENS_TILE_ANALOG_PARAMETERS(X)                                                        \
    X(std::uint32_t, analogArrayCount,                                                           \
      "analog_array_count", 0, "hardware",                                                       \
      "Simulation-wide number of analog arrays instantiated on every tile; zero "                \
      "disables analog",                                                                         \
      "0")                                                                                       \
    X(std::uint32_t, analogArrayRows,                                                            \
      "analog_array_rows", 100, "hardware",                                                      \
      "Simulation-wide row count shared by every analog array",                                  \
      "100")                                                                                     \
    X(std::uint32_t, analogArrayColumns,                                                         \
      "analog_array_columns", 100, "hardware",                                                   \
      "Simulation-wide column count shared by every analog array",                               \
      "100")                                                                                     \
    X(std::string, analogBackend,                                                                \
      "analog_backend", "native", "hardware",                                                    \
      "Analog numerical backend: native or crosssim",                                            \
      "native")                                                                                  \
    X(std::string, crossSimConfig,                                                               \
      "crosssim_config", "", "workload",                                                         \
      "Optional CrossSim JSON parameter file used by this tile's independent backend",           \
      "")                                                                                        \
    X(std::string, analogLinkClock,                                                              \
      "analog_link_clock", "1GHz", "hardware",                                                   \
      "Clock for the tile-wide shared bidirectional 256-bit analog link",                        \
      "1GHz")                                                                                    \
    X(std::uint64_t, analogComputeLatencyCycles,                                                 \
      "analog_compute_latency_cycles", 100, "hardware",                                          \
      "Analog compute latency in analog-link cycles",                                            \
      "100")

// Measurement and diagnostics.
#define MITTENS_TILE_MEASUREMENT_PARAMETERS(X)                                                   \
    X(std::string, profileMode,                                                                  \
      "profile_mode", "off", "measurement",                                                      \
      "Performance profiling mode: off, summary, or trace",                                      \
      "off")                                                                                     \
    X(std::string, profileOutputDirectory,                                                       \
      "profile_output_directory", "", "measurement",                                             \
      "Directory for per-tile performance profile files",                                        \
      "")                                                                                        \
    X(std::uint64_t, progressSnapshotIntervalMilliseconds,                                       \
      "progress_snapshot_interval_ms", 10000, "measurement",                                     \
      "Wall-time interval for bounded progress snapshots; zero disables periodic "               \
      "snapshots",                                                                               \
      "10000")                                                                                   \
    X(std::uint64_t, progressWatchdogMilliseconds,                                               \
      "progress_watchdog_ms", 60000, "measurement",                                              \
      "Wall-time interval without deployment-wide architectural progress on this SST "           \
      "rank before dumping diagnostics and failing; zero disables the watchdog",                 \
      "60000")                                                                                   \
    X(std::string, taskTraceDirectory,                                                           \
      "task_trace_directory", "", "measurement",                                                 \
      "Optional directory for per-tile task trace CSV files",                                    \
      "")                                                                                        \
    X(std::string, serialOutputDirectory,                                                        \
      "serial_output_directory", "", "measurement",                                              \
      "Optional directory for isolated per-tile QEMU UART logs",                                 \
      "")                                                                                        \
    X(int, verbosity,                                                                            \
      "verbose", 0, "measurement",                                                               \
      "Mittens diagnostic verbosity",                                                            \
      "0")

// Stable public expansion used by configuration parsing, generated fields, and
// SST ELI parameter documentation.
#define MITTENS_TILE_PARAMETERS(X)                                                                   \
    MITTENS_TILE_TOPOLOGY_PARAMETERS(X)                                                              \
    MITTENS_TILE_MEMORY_PARAMETERS(X)                                                                \
    MITTENS_TILE_INSTRUCTION_PARAMETERS(X)                                                           \
    MITTENS_TILE_SCRATCHPAD_PARAMETERS(X)                                                            \
    MITTENS_TILE_GLOBAL_PARAMETERS(X)                                                                \
    MITTENS_TILE_CPU_PARAMETERS(X)                                                                   \
    MITTENS_TILE_RVV_PARAMETERS(X)                                                                   \
    MITTENS_TILE_DMA_PARAMETERS(X)                                                                   \
    MITTENS_TILE_ANALOG_PARAMETERS(X)                                                                \
    MITTENS_TILE_MEASUREMENT_PARAMETERS(X)
