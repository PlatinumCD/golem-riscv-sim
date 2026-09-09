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
      "16")                                                                                      \
    X(bool, networkTailDelivery,                                                                 \
      "network_tail_delivery", false, "hardware",                                                \
      "The network interface delivers a request only after its tail flit arrives",               \
      "false")

// Guest and data-memory configuration.
#define MITTENS_TILE_MEMORY_PARAMETERS(X)                                                        \
    X(std::string, qemuPath,                                                                     \
      "qemu_path", "qemu-system-riscv64", "execution",                                           \
      "Path to the QEMU system emulator",                                                        \
      "qemu-system-riscv64")                                                                     \
    X(std::string, elfPath,                                                                      \
      "elf", "", "workload",                                                                     \
      "Bare-metal ELF image loaded by QEMU",                                                     \
      "")                                                                                        \
    X(std::string, memory,                                                                       \
      "memory", "16M", "hardware",                                                               \
      "Small non-architectural QEMU control memory assigned to this tile",                       \
      "16M")                                                                                     \
    X(std::string, memoryBackend,                                                                \
      "memory_backend", "native", "hardware",                                                    \
      "Data-memory timing backend: native, memhierarchy, or streaming",                          \
      "native")                                                                                  \
    X(std::uint64_t, memoryGuestBase,                                                            \
      "memory_guest_base", 0, "hardware",                                                        \
      "Guest physical base used for tile-namespaced timing addresses; zero preserves "           \
      "guest addresses",                                                                         \
      "0")                                                                                       \
    X(std::uint64_t, memoryTileStride,                                                           \
      "memory_tile_stride", 0, "hardware",                                                       \
      "Per-tile timing-address stride; zero preserves guest addresses",                          \
      "0")                                                                                       \
    X(std::uint32_t, memoryCacheLineSize,                                                        \
      "memory_cache_line_size", 64, "hardware",                                                  \
      "Cache line size used for receive-DMA invalidation",                                       \
      "64")                                                                                      \
    X(std::uint32_t, memoryLoadQueueEntries,                                                     \
      "memory_load_queue_entries", 8, "hardware",                                                \
      "Timed load-queue capacity for grouped nonblocking loads",                                 \
      "8")                                                                                       \
    X(std::uint32_t, memoryStoreBufferEntries,                                                   \
      "memory_store_buffer_entries", 1, "hardware",                                              \
      "Timed CPU store-buffer capacity; one preserves blocking behavior",                        \
      "1")

// Replay batching and initialization.
#define MITTENS_TILE_EXECUTION_PARAMETERS(X)                                                     \
    X(bool, memoryInitializationBatching,                                                        \
      "memory_init_batching", false, "execution",                                                \
      "Aggregate pre-runtime data accesses into one fd 41 initialization handshake",             \
      "false")                                                                                   \
    X(bool, memoryAccessBatching,                                                                \
      "memory_access_batching", false, "execution",                                              \
      "Batch fd 41 transport while replaying each ordinary runtime access through "              \
      "MemHierarchy",                                                                            \
      "false")                                                                                   \
    X(bool, scratchpadAccessBatching,                                                            \
      "scratchpad_access_batching", false, "execution",                                          \
      "Batch fd 41 transport while replaying every runtime scratchpad access through "           \
      "ScratchpadTimingModel",                                                                   \
      "false")                                                                                   \
    X(bool, scratchpadAccessRunCompaction,                                                       \
      "scratchpad_access_run_compaction", false, "execution",                                    \
      "Compact adjacent fragments from one dynamic scratchpad instruction while "                \
      "preserving logical timing and accounting",                                                \
      "false")                                                                                   \
    X(bool, memoryEventBatching,                                                                 \
      "memory_event_batching", false, "execution",                                               \
      "Fuse a pending memory-access batch with its immediately following "                       \
      "synchronization event",                                                                   \
      "false")                                                                                   \
    X(bool, globalDMASubmitBatching,                                                             \
      "global_dma_submit_batching", false, "execution",                                          \
      "Batch ordered nonblocking global-RAM DMA submissions across fd 41 while "                 \
      "preserving each physical descriptor and modeled CPU boundary",                            \
      "false")                                                                                   \
    X(bool, globalDMAMacroExecution,                                                             \
      "global_dma_macro_execution", false, "execution",                                          \
      "Replay compiler-certified ordered global-RAM DMA event tapes from one fd-41 "             \
      "envelope without changing physical timing",                                               \
      "false")                                                                                   \
    X(bool, analogCommandBatching,                                                               \
      "analog_command_batching", false, "execution",                                             \
      "Batch proven nonblocking analog submissions across fd 41 while replaying every"           \
      " command at its original modeled CPU boundary",                                           \
      "false")                                                                                   \
    X(std::uint32_t, memoryAccessBatchRecords,                                                   \
      "memory_access_batch_records", 16, "execution",                                            \
      "Maximum logical memory accesses before flushing one bounded fd 41 record batch",          \
      "16")                                                                                      \
    X(std::uint32_t, memoryInitializationBytesPerCycle,                                          \
      "memory_init_bytes_per_cycle", 32, "hardware",                                             \
      "Aggregate initialization bandwidth in bytes per CPU cycle",                               \
      "32")                                                                                      \
    X(std::uint64_t, memoryInitializationLatencyCycles,                                          \
      "memory_init_latency_cycles", 2, "hardware",                                               \
      "One-time aggregate initialization latency in CPU cycles",                                 \
      "2")                                                                                       \
    X(std::uint64_t, memoryInitializationInstructionQuantum,                                     \
      "memory_init_instruction_quantum", UINT64_C(67108864), "execution",                        \
      "Maximum instructions SST grants QEMU during pre-runtime initialization",                  \
      "67108864")                                                                                \
    X(std::uint32_t, memoryInitializationBarrierTiles,                                           \
      "memory_init_barrier_tiles", 0, "hardware",                                                \
      "Number of active tiles that must finish pre-runtime initialization before any "           \
      "tile enters runtime; zero disables the deployment barrier",                               \
      "0")

// Private noncoherent scratchpad.
#define MITTENS_TILE_SCRATCHPAD_PARAMETERS(X)                                                    \
    X(bool, scratchpadEnabled,                                                                   \
      "scratchpad_enabled", false, "hardware",                                                   \
      "Enable the private noncoherent tile scratchpad",                                          \
      "false")                                                                                   \
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
      "Scalar front-end issue width: 1, 2, or 4 instructions per cycle; vector issue "           \
      "remains one per cycle",                                                                   \
      "1")                                                                                       \
    X(std::uint64_t, syncInstructionQuantum,                                                     \
      "sync_instruction_quantum", 1000, "execution",                                             \
      "Maximum instructions SST grants QEMU at once",                                            \
      "1000")                                                                                    \
    X(std::uint32_t, qemuReadySetWorkers,                                                        \
      "qemu_ready_set_workers", 1, "execution",                                                  \
      "Bounded host worker count for deterministic same-frontier initial QEMU grants;"           \
      " one preserves serial execution",                                                         \
      "1")                                                                                       \
    X(bool, qemuRuntimeReadySet,                                                                 \
      "qemu_runtime_ready_set", false, "execution",                                              \
      "Use the deterministic ready-set executor for independently runnable runtime "             \
      "QEMU captures",                                                                           \
      "false")                                                                                   \
    X(bool, qemuLocalLookahead,                                                                  \
      "qemu_local_lookahead", false, "execution",                                                \
      "Compute the next QEMU stop asynchronously after a standalone private "                    \
      "scratchpad batch while preserving its modeled commit frontier",                           \
      "false")                                                                                   \
    X(std::uint32_t, qemuCaptureSpinMicroseconds,                                                \
      "qemu_capture_spin_us", 0, "execution",                                                    \
      "Host-only busy-poll interval before an SST-to-QEMU capture falls back to futex"           \
      " sleep",                                                                                  \
      "0")                                                                                       \
    X(std::string, qemuReadySetIndependenceProof,                                                \
      "qemu_ready_set_independence_proof", "", "execution",                                      \
      "Frozen materialization-audit SHA-256 required when initial QEMU grants run "              \
      "concurrently",                                                                            \
      "")

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
      "Bounded source-Scratchpad-to-NIC FIFO capacity in bytes; zero disables timed "            \
      "TX DMA",                                                                                  \
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
    MITTENS_TILE_EXECUTION_PARAMETERS(X)                                                             \
    MITTENS_TILE_SCRATCHPAD_PARAMETERS(X)                                                            \
    MITTENS_TILE_GLOBAL_PARAMETERS(X)                                                                \
    MITTENS_TILE_CPU_PARAMETERS(X)                                                                   \
    MITTENS_TILE_RVV_PARAMETERS(X)                                                                   \
    MITTENS_TILE_DMA_PARAMETERS(X)                                                                   \
    MITTENS_TILE_ANALOG_PARAMETERS(X)                                                                \
    MITTENS_TILE_MEASUREMENT_PARAMETERS(X)
