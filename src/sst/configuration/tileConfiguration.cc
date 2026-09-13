#include "sst_config.h"
#include "tileConfiguration.h"
#include "resolvedConfiguration.h"
#include <sst/core/params.h>
#include <sst/core/output.h>
#include <mittens/NICTileBridge.h>
#include <algorithm>
#include <stdexcept>

namespace SST::Mittens
{
namespace
{
constexpr std::size_t kDeploymentFrameHeaderWords = 7;
bool isPowerOfTwo(std::uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

TileConfiguration TileConfiguration::read(SST::Params& params)
{
    TileConfiguration config;
    auto fixed = [&](const char* name, auto expected) {
        using Value = decltype(expected);
        if (params.find<Value>(name, expected) != expected)
            throw std::invalid_argument(std::string("fixed execution setting: ") + name);
    };
    fixed("memory", std::string("16M"));
    fixed("memory_backend", std::string("streaming"));
    fixed("memory_init_batching", bool(false));
    fixed("memory_access_batching", bool(false));
    fixed("scratchpad_access_batching", bool(false));
    fixed("scratchpad_access_run_compaction", bool(false));
    fixed("memory_event_batching", bool(false));
    fixed("global_dma_submit_batching", bool(false));
    fixed("global_dma_macro_execution", bool(false));
    fixed("analog_command_batching", bool(false));
    fixed("memory_access_batch_records", std::uint32_t(16));
    fixed("memory_init_bytes_per_cycle", std::uint32_t(32));
    fixed("memory_init_latency_cycles", std::uint64_t(2));
    fixed("memory_init_instruction_quantum", std::uint64_t(UINT64_C(67108864)));
    fixed("network_tail_delivery", bool(true));
    fixed("memory_init_barrier_tiles", std::uint32_t(0));
    fixed("scratchpad_boot", bool(true));
    fixed("scratchpad_enabled", bool(true));
    fixed("qemu_ready_set_workers", std::uint32_t(1));
    fixed("qemu_runtime_ready_set", bool(false));
    fixed("qemu_local_lookahead", bool(false));
    fixed("qemu_ready_set_independence_proof", std::string(""));

#define MITTENS_READ(type, field, name, value, category, help, documented)                         \
    config.field = params.find<type>(name, value);
    MITTENS_TILE_PARAMETERS(MITTENS_READ)
#undef MITTENS_READ
    if (config.profileMode == "trace" && config.taskTraceDirectory.empty())
        config.taskTraceDirectory = config.profileOutputDirectory;
    return config;
}

void TileConfiguration::validate(SST::Output& output_) const
{
    const auto& config_ = *this;
    if (!config_.scratchpadBoot || !config_.scratchpadEnabled || config_.memoryBackend != "streaming")
        output_.fatal(CALL_INFO, -1,
                      "only executable SPM with an instruction cache and streaming DMA is supported\n");
    if ((config_.meshWidth == 0) != (config_.meshHeight == 0))
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires both mesh_width and mesh_height, or neither\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.meshWidth != 0 &&
        (static_cast<std::uint64_t>(config_.meshWidth) * config_.meshHeight !=
             config_.networkSize ||
         config_.tileId >= config_.networkSize))
    {
        output_.fatal(
            CALL_INFO, -1, "tile %u mesh dimensions %ux%u do not match network_size %u\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(config_.meshWidth),
            static_cast<unsigned>(config_.meshHeight), static_cast<unsigned>(config_.networkSize));
    }
    if (config_.meshLinkClock.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty mesh_link_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.meshLinkWidthBits == 0 || config_.meshLinkWidthBits % 32 != 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires mesh_link_width_bits to be a positive "
                      "multiple of 32\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.networkPacketWords < kDeploymentFrameHeaderWords)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires network_packet_words to be at least %zu "
                      "so one deployment header stays in one packet\n",
                      static_cast<unsigned>(config_.tileId), kDeploymentFrameHeaderWords);
    }
    if (config_.qemuPath.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty qemu_path\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.memory.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty memory setting\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.scratchpadBoot &&
        (!config_.scratchpadEnabled || config_.memoryBackend != "streaming" ||
         config_.memoryInitializationBatching || config_.memoryAccessBatching ||
         config_.scratchpadAccessBatching || config_.memoryEventBatching ||
         config_.globalDMASubmitBatching || config_.globalDMAMacroExecution ||
         config_.analogCommandBatching || config_.cpuIssueWidth != 1 ||
         config_.qemuReadySetWorkers != 1 || config_.qemuRuntimeReadySet ||
         config_.qemuLocalLookahead))
    {
        output_.fatal(CALL_INFO, -1,
                      "scratchpad_boot requires scratchpad_enabled, streaming memory, "
                      "and unbatched single-issue execution without QEMU lookahead/ready sets\n");
    }
    if (config_.scratchpadBoot &&
        (!isPowerOfTwo(config_.instructionCacheBytes) ||
         !isPowerOfTwo(config_.instructionCacheLineBytes) ||
         !isPowerOfTwo(config_.instructionCacheWays) ||
         config_.instructionCacheLineBytes < 4 ||
         static_cast<std::uint64_t>(config_.instructionCacheLineBytes) *
             config_.instructionCacheWays > config_.instructionCacheBytes ||
         config_.instructionCacheLineBytes > config_.scratchpadBytes ||
         config_.scratchpadBytes % config_.instructionCacheLineBytes != 0 ||
         config_.instructionCacheHitCycles == 0))
    {
        output_.fatal(CALL_INFO, -1, "invalid scratchpad instruction-cache geometry or latency\n");
    }
    if (config_.launchMode != "disabled" && config_.launchMode != "managed")
    {
        output_.fatal(CALL_INFO, -1, "tile %u has unsupported launch_mode '%s'\n",
                      static_cast<unsigned>(config_.tileId), config_.launchMode.c_str());
    }

    if (config_.launchMode == "managed" && config_.elfPath.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires an ELF in managed launch mode\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.epochBarrierEpochs != 0 && config_.launchMode != "managed")
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires managed launch when the modeled epoch barrier is enabled\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.cpuClock.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty cpu_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.cpuIssueWidth != 1)
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires single-issue execution (cpu_issue_width=1)\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.syncInstructionQuantum == 0)
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires sync_instruction_quantum to be nonzero\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.qemuCaptureSpinMicroseconds > 1000000)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires qemu_capture_spin_us to be at most 1000000\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.globalRAMBytes == 0 || config_.globalRAMBytes > INT64_MAX)
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an invalid global_ram_bytes capacity\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        (!isPowerOfTwo(config_.riscvVectorLengthBits) || config_.riscvVectorLengthBits < 128 ||
         config_.riscvVectorLengthBits > 1024))
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires riscv_vector_length_bits to be a power "
                      "of two in [128, 1024]\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        (!isPowerOfTwo(config_.riscvVectorElementBits) || config_.riscvVectorElementBits < 8 ||
         config_.riscvVectorElementBits > 64))
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires riscv_vector_element_bits to be a power "
                      "of two in [8, 64]\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        config_.riscvVectorElementBits > config_.riscvVectorLengthBits)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires riscv_vector_element_bits not to exceed "
                      "riscv_vector_length_bits\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAStreams != 1 && config_.receiveDMAStreams != 2 && config_.receiveDMAStreams != 4)
        output_.fatal(CALL_INFO, -1, "rx_dma_streams must be 1, 2, or 4\n");
    if (config_.receiveDMAClock.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty rx_dma_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAWidthBits == 0 || config_.receiveDMAWidthBits % 32 != 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires rx_dma_width_bits to be a positive "
                      "multiple of 32\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.scratchpadEnabled &&
        (config_.receiveDMAWidthBits / 8 != config_.scratchpadDMABytesPerCycle ||
         config_.receiveDMASetupCycles != config_.scratchpadDMASetupCycles))
    {
        output_.fatal(
            CALL_INFO, -1,
            "tile %u requires RX-DMA width/setup to match the common scratchpad DMA model\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAQueueDepth == 0 ||
        config_.receiveDMAQueueDepth > MITTENS_BRIDGE_BURST_QUEUE_CAPACITY)
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires rx_dma_queue_depth in [1, %u]\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(MITTENS_BRIDGE_BURST_QUEUE_CAPACITY));
    }
    if (config_.transmitDMAFIFOBytes % sizeof(std::uint32_t) != 0 ||
        config_.transmitDMAFIFOBytes < config_.scratchpadDMABytesPerCycle)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires tx_dma_fifo_bytes to be a word-aligned value at "
                      "least one scratchpad DMA beat\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.transmitDMAStreams != 1 && config_.transmitDMAStreams != 2 &&
        config_.transmitDMAStreams != 4)
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires tx_dma_streams in {1,2,4}\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogBackend != "native" && config_.analogBackend != "timing" &&
        config_.analogBackend != "crosssim")
    {
        output_.fatal(
            CALL_INFO, -1,
            "tile %u has unsupported analog_backend '%s'; expected native, timing, or crosssim\n",
            static_cast<unsigned>(config_.tileId), config_.analogBackend.c_str());
    }
    if (config_.analogLinkClock.empty())
    {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty analog_link_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogComputeLatencyCycles == 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires analog_compute_latency_cycles to be nonzero\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogCommandBatching && config_.analogArrayCount == 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires analog arrays when analog_command_batching "
                      "is enabled\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogArrayRows == 0)
    {
        output_.fatal(CALL_INFO, -1, "tile %u has zero analog_array_rows\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.analogArrayColumns == 0)
    {
        output_.fatal(CALL_INFO, -1, "tile %u has zero analog_array_columns\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.profileMode != "off" && config_.profileMode != "summary" &&
        config_.profileMode != "trace")
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has unsupported profile_mode '%s'; expected off, "
                      "summary, or trace\n",
                      static_cast<unsigned>(config_.tileId), config_.profileMode.c_str());
    }
    if (config_.profileMode != "off" && config_.profileOutputDirectory.empty())
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires profile_output_directory when profiling is "
                      "enabled\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.verbosity < 0)
    {
        output_.fatal(CALL_INFO, -1, "tile %u has a negative verbosity\n",
                      static_cast<unsigned>(config_.tileId));
    }
}

std::string TileConfiguration::writeResolved(const std::string& directory) const
{
    return writeResolvedConfiguration(directory, "tile", tileId,
                                      [&](ConfigurationWriter& out)
                                      {
#define MITTENS_WRITE(type, field, name, value, category, help, documented)                        \
    out.add(name, category, field);
                                          MITTENS_TILE_PARAMETERS(MITTENS_WRITE)
#undef MITTENS_WRITE
                                      });
}

} // namespace SST::Mittens
