#include "sst_config.h"

#include "tile.h"

#include "analog/crossSimAnalogBackend.h"
#include "analog/nativeAnalogBackend.h"
#include "packetEvent.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>

namespace SST {
namespace Mittens {

namespace {

constexpr auto kSyncWaitTimeout = std::chrono::milliseconds(50);
constexpr std::uint32_t kDeploymentFrameMagic = UINT32_C(0x474f4c4d);
constexpr std::size_t kDeploymentFrameHeaderWords = 5;

bool isPowerOfTwo(std::uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t divideRoundUp(
    std::uint64_t value,
    std::uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

const char* syncStopReasonName(std::uint32_t reason)
{
    switch (reason) {
    case MITTENS_SYNC_STOP_NONE:
        return "none";
    case MITTENS_SYNC_STOP_QUANTUM_END:
        return "quantum-end";
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
        return "nic-transmit";
    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
        return "nic-receive-wait";
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
        return "analog-submit";
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
        return "analog-wait";
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        return "guest-exit";
    case MITTENS_SYNC_STOP_TASK_START:
        return "task-start";
    case MITTENS_SYNC_STOP_TASK_FINISH:
        return "task-finish";
    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
        return "nic-rx-dma-submit";
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
        return "memory-access";
    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE:
        return "memory-init-complete";
    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
        return "nic-transmit-wait";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
        return "scratchpad-dma-submit";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
        return "scratchpad-dma-wait";
    default:
        return "unknown";
    }
}

} // namespace

Tile::Configuration Tile::readConfiguration(SST::Params& params)
{
    Configuration configuration{
        params.find<std::uint32_t>("tile_id", 0),
        params.find<std::uint32_t>("network_size", 0),
        params.find<std::uint32_t>("mesh_width", 0),
        params.find<std::uint32_t>("mesh_height", 0),
        params.find<std::string>("mesh_link_clock", "1GHz"),
        params.find<std::uint32_t>("mesh_link_width_bits", 32),
        params.find<std::string>("qemu_path", "qemu-system-riscv64"),
        params.find<std::string>("elf", ""),
        params.find<std::string>("memory", "16M"),
        params.find<std::string>("memory_backend", "native"),
        params.find<std::uint64_t>("memory_guest_base", 0),
        params.find<std::uint64_t>("memory_tile_stride", 0),
        params.find<std::uint32_t>("memory_cache_line_size", 64),
        params.find<std::uint32_t>(
            "memory_store_buffer_entries", 1),
        params.find<bool>("memory_init_batching", false),
        params.find<std::uint32_t>(
            "memory_init_bytes_per_cycle", 32),
        params.find<std::uint64_t>(
            "memory_init_latency_cycles", 2),
        params.find<bool>("scratchpad_enabled", false),
        params.find<std::uint64_t>("scratchpad_bytes", 256 * 1024),
        params.find<std::uint32_t>("scratchpad_banks", 8),
        params.find<std::uint32_t>("scratchpad_read_ports", 1),
        params.find<std::uint32_t>("scratchpad_write_ports", 1),
        params.find<std::uint32_t>("scratchpad_access_width_bits", 256),
        params.find<std::uint64_t>("scratchpad_latency_cycles", 1),
        params.find<std::uint32_t>("scratchpad_dma_bytes_per_cycle", 32),
        params.find<std::uint64_t>("scratchpad_dma_setup_cycles", 8),
        params.find<std::string>("launch_mode", "disabled"),
        params.find<std::string>("cpu_clock", "1GHz"),
        params.find<std::uint32_t>("cpu_issue_width", 1),
        params.find<std::uint64_t>("sync_instruction_quantum", 1000),
        params.find<bool>("riscv_vector_enabled", true),
        params.find<std::uint32_t>("riscv_vector_length_bits", 256),
        params.find<std::uint32_t>("riscv_vector_element_bits", 64),
        params.find<std::string>("rx_dma_clock", "1GHz"),
        params.find<std::uint32_t>("rx_dma_width_bits", 256),
        params.find<std::uint64_t>("rx_dma_setup_cycles", 8),
        params.find<std::uint32_t>("rx_dma_queue_depth", 4),
        params.find<std::uint32_t>("analog_array_count", 0),
        params.find<std::uint32_t>("analog_array_rows", 100),
        params.find<std::uint32_t>("analog_array_columns", 100),
        params.find<std::string>("analog_backend", "native"),
        params.find<std::string>("crosssim_config", ""),
        params.find<std::string>("analog_link_clock", "1GHz"),
        params.find<std::uint64_t>("analog_compute_latency_cycles", 100),
        params.find<std::string>("profile_mode", "off"),
        params.find<std::string>("profile_output_directory", ""),
        params.find<std::string>("task_trace_directory", ""),
        params.find<int>("verbose", 0),
    };
    if (configuration.profileMode == "trace" &&
        configuration.taskTraceDirectory.empty()) {
        configuration.taskTraceDirectory =
            configuration.profileOutputDirectory;
    }
    return configuration;
}

Tile::Tile(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    config_(readConfiguration(params)),
    output_("mittens: ", config_.verbosity, 0, SST::Output::STDOUT),
    network_(nullptr),
    memoryInterface_(nullptr),
    receiveDMAEngine_(
        config_.receiveDMAWidthBits,
        config_.receiveDMASetupCycles),
    state_(LifecycleState::Constructed)
{
    validateConfiguration();
    if (config_.scratchpadEnabled) {
        try {
            scratchpadTimingModel_ =
                std::make_unique<ScratchpadTimingModel>(
                    ScratchpadTimingConfiguration{
                        config_.scratchpadBytes,
                        config_.scratchpadBanks,
                        config_.scratchpadReadPorts,
                        config_.scratchpadWritePorts,
                        config_.scratchpadAccessWidthBits,
                        config_.scratchpadLatencyCycles,
                        config_.scratchpadDMASetupCycles,
                        config_.scratchpadDMABytesPerCycle,
                    });
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u has an invalid scratchpad configuration: %s\n",
                static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    try {
        performanceProfile_.configure(
            config_.tileId,
            config_.profileMode == "off"
                ? std::string()
                : config_.profileOutputDirectory,
            config_.profileMode == "trace");
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u could not configure performance profiling: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }

    network_ = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
        "networkIF", SST::ComponentInfo::SHARE_NONE, 1);

    if (network_ != nullptr && !managedLaunch()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires managed launch when networkIF is attached\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (network_ != nullptr && config_.networkSize == 0) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires a nonzero network_size when networkIF is attached\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (network_ != nullptr) {
        network_->setNotifyOnSend(
            new SST::Interfaces::SimpleNetwork::Handler<
                Tile, &Tile::handleNetworkSend>(this));
        network_->setNotifyOnReceive(
            new SST::Interfaces::SimpleNetwork::Handler<
                Tile, &Tile::handleNetworkReceive>(this));
    }

    if (config_.memoryBackend == "memhierarchy") {
        memoryClockTimeBase_ = getTimeConverter(config_.cpuClock);
        memoryInterface_ =
            loadUserSubComponent<SST::Interfaces::StandardMem>(
                "memoryIF",
                SST::ComponentInfo::SHARE_NONE,
                memoryClockTimeBase_,
                new SST::Interfaces::StandardMem::Handler<
                    Tile, &Tile::handleMemoryResponse>(this));
        if (memoryInterface_ == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u requires a StandardMem memoryIF when "
                "memory_backend=memhierarchy\n",
                static_cast<unsigned>(config_.tileId));
        }
    }
    if (config_.networkSize != 0 && config_.tileId >= config_.networkSize) {
        output_.fatal(CALL_INFO, -1,
                      "tile ID %u is outside network_size %u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(config_.networkSize));
    }

    if (managedLaunch()) {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
        cpuSyncLink_ = configureSelfLink(
            "qemu-sync",
            config_.cpuClock,
            new SST::Event::Handler<
                Tile, &Tile::handleCpuSyncEvent>(this));
        if (cpuSyncLink_ == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u failed to configure its QEMU synchronization link\n",
                static_cast<unsigned>(config_.tileId));
        }
        if (network_ != nullptr) {
            networkClockTimeBase_ =
                getTimeConverter(config_.meshLinkClock);
            networkCompletionLink_ = configureSelfLink(
                "network-completion",
                networkClockTimeBase_,
                new SST::Event::Handler<
                    Tile,
                    &Tile::handleNetworkCompletionEvent>(this));
            if (networkCompletionLink_ == nullptr) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u failed to configure its network "
                    "completion link\n",
                    static_cast<unsigned>(config_.tileId));
            }
            receiveDMAClockTimeBase_ =
                getTimeConverter(config_.receiveDMAClock);
            receiveDMALink_ = configureSelfLink(
                "rx-dma",
                receiveDMAClockTimeBase_,
                new SST::Event::Handler<
                    Tile, &Tile::handleReceiveDMAEvent>(this));
            if (receiveDMALink_ == nullptr) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u failed to configure its receive DMA link\n",
                    static_cast<unsigned>(config_.tileId));
            }
        }
    }

    if (config_.analogArrayCount != 0) {
        std::unique_ptr<AnalogBackend> backend;
        try {
            if (config_.analogBackend == "native") {
                backend = std::make_unique<NativeAnalogBackend>(
                    config_.analogArrayCount,
                    config_.analogArrayRows,
                    config_.analogArrayColumns);
            } else {
                backend = std::make_unique<CrossSimAnalogBackend>(
                    config_.analogArrayCount,
                    config_.analogArrayRows,
                    config_.analogArrayColumns,
                    config_.crossSimConfig);
            }
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u failed to initialize analog_backend '%s': %s\n",
                static_cast<unsigned>(config_.tileId),
                config_.analogBackend.c_str(),
                error.what());
        }

        analogDevice_ = std::make_unique<AnalogDevice>(
            config_.tileId,
            config_.analogComputeLatencyCycles,
            std::move(backend));
        analogDevice_->setTraceEnabled(
            performanceProfile_.traceEnabled());

        analogClockHandler_ =
            new SST::Clock::Handler<Tile, &Tile::clockAnalog>(this);
        analogClockTimeBase_ =
            registerClock(config_.analogLinkClock, analogClockHandler_);
        analogClockRegistered_ = true;
    }

    output_.verbose(
        CALL_INFO,
        1,
        0,
        "configured tile %u (qemu=%s, elf=%s, memory=%s, memory_backend=%s/init-batch-%s/%uB-cycle/setup-%llu, launch=%s, cpu_clock=%s, issue_width=%u, sync_quantum=%llu, rvv=%s/vlen-%u/elen-%u, network=%s, network_size=%u, mesh_link=%s/%u-bit, rx_dma=%s/%u-bit/setup-%llu/queue-%u)\n",
        static_cast<unsigned>(config_.tileId),
        config_.qemuPath.c_str(),
        config_.elfPath.empty() ? "<unset>" : config_.elfPath.c_str(),
        config_.memory.c_str(),
        config_.memoryBackend.c_str(),
        config_.memoryInitializationBatching ? "on" : "off",
        static_cast<unsigned>(
            config_.memoryInitializationBytesPerCycle),
        static_cast<unsigned long long>(
            config_.memoryInitializationLatencyCycles),
        config_.launchMode.c_str(),
        config_.cpuClock.c_str(),
        static_cast<unsigned>(config_.cpuIssueWidth),
        static_cast<unsigned long long>(
            config_.syncInstructionQuantum),
        config_.riscvVectorEnabled ? "enabled" : "disabled",
        static_cast<unsigned>(config_.riscvVectorLengthBits),
        static_cast<unsigned>(config_.riscvVectorElementBits),
        network_ == nullptr ? "detached" : "attached",
        static_cast<unsigned>(config_.networkSize),
        config_.meshLinkClock.c_str(),
        static_cast<unsigned>(config_.meshLinkWidthBits),
        config_.receiveDMAClock.c_str(),
        static_cast<unsigned>(config_.receiveDMAWidthBits),
        static_cast<unsigned long long>(
            config_.receiveDMASetupCycles),
        static_cast<unsigned>(config_.receiveDMAQueueDepth));

    output_.verbose(
        CALL_INFO,
        1,
        0,
        "configured tile %u scratchpad (%s, bytes=%llu, banks=%u, "
        "ports=%uR/%uW, width=%u-bit, latency=%llu cycles, "
        "dma=%uB-cycle/setup-%llu)\n",
        static_cast<unsigned>(config_.tileId),
        config_.scratchpadEnabled ? "enabled" : "disabled",
        static_cast<unsigned long long>(config_.scratchpadBytes),
        static_cast<unsigned>(config_.scratchpadBanks),
        static_cast<unsigned>(config_.scratchpadReadPorts),
        static_cast<unsigned>(config_.scratchpadWritePorts),
        static_cast<unsigned>(config_.scratchpadAccessWidthBits),
        static_cast<unsigned long long>(
            config_.scratchpadLatencyCycles),
        static_cast<unsigned>(config_.scratchpadDMABytesPerCycle),
        static_cast<unsigned long long>(
            config_.scratchpadDMASetupCycles));

    if (analogDevice_ != nullptr) {
        output_.verbose(
            CALL_INFO,
            1,
            0,
            "configured tile %u analog device (%zu arrays, size=%ux%u, backend=%s, shared_link=%s, width=%u bits, compute_latency=%llu cycles)\n",
            static_cast<unsigned>(config_.tileId),
            analogDevice_->arrayCount(),
            static_cast<unsigned>(config_.analogArrayRows),
            static_cast<unsigned>(config_.analogArrayColumns),
            config_.analogBackend.c_str(),
            config_.analogLinkClock.c_str(),
            static_cast<unsigned>(MITTENS_ANALOG_LINK_WIDTH_BITS),
            static_cast<unsigned long long>(
                config_.analogComputeLatencyCycles));
    }
}

void Tile::validateConfiguration() const
{
    if ((config_.meshWidth == 0) != (config_.meshHeight == 0)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires both mesh_width and mesh_height, or neither\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.meshWidth != 0 &&
        (static_cast<std::uint64_t>(config_.meshWidth) *
             config_.meshHeight !=
         config_.networkSize ||
         config_.tileId >= config_.networkSize)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u mesh dimensions %ux%u do not match network_size %u\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(config_.meshWidth),
            static_cast<unsigned>(config_.meshHeight),
            static_cast<unsigned>(config_.networkSize));
    }
    if (config_.meshLinkClock.empty()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u has an empty mesh_link_clock\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.meshLinkWidthBits == 0 ||
        config_.meshLinkWidthBits % 32 != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires mesh_link_width_bits to be a positive "
            "multiple of 32\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.qemuPath.empty()) {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty qemu_path\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.memory.empty()) {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty memory setting\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.memoryBackend != "native" &&
        config_.memoryBackend != "memhierarchy") {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u has unsupported memory_backend '%s'\n",
            static_cast<unsigned>(config_.tileId),
            config_.memoryBackend.c_str());
    }
    if (config_.memoryInitializationBatching &&
        config_.memoryBackend != "memhierarchy") {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires memory_backend=memhierarchy when "
            "memory_init_batching is enabled\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.memoryCacheLineSize == 0 ||
        (config_.memoryCacheLineSize &
         (config_.memoryCacheLineSize - 1)) != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires memory_cache_line_size to be a "
            "power of two\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.memoryStoreBufferEntries == 0 ||
        config_.memoryStoreBufferEntries > 64) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires memory_store_buffer_entries in [1, 64]\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.memoryInitializationBytesPerCycle == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires memory_init_bytes_per_cycle to be "
            "nonzero\n",
            static_cast<unsigned>(config_.tileId));
    }

    if (config_.launchMode != "disabled" && config_.launchMode != "managed") {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has unsupported launch_mode '%s'\n",
                      static_cast<unsigned>(config_.tileId),
                      config_.launchMode.c_str());
    }

    if (config_.launchMode == "managed" && config_.elfPath.empty()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires an ELF in managed launch mode\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.cpuClock.empty()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has an empty cpu_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.cpuIssueWidth != 1 &&
        config_.cpuIssueWidth != 2 &&
        config_.cpuIssueWidth != 4) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires cpu_issue_width to be 1, 2, or 4\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.syncInstructionQuantum == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires sync_instruction_quantum to be nonzero\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        (!isPowerOfTwo(config_.riscvVectorLengthBits) ||
         config_.riscvVectorLengthBits < 128 ||
         config_.riscvVectorLengthBits > 1024)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires riscv_vector_length_bits to be a power "
            "of two in [128, 1024]\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        (!isPowerOfTwo(config_.riscvVectorElementBits) ||
         config_.riscvVectorElementBits < 8 ||
         config_.riscvVectorElementBits > 64)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires riscv_vector_element_bits to be a power "
            "of two in [8, 64]\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.riscvVectorEnabled &&
        config_.riscvVectorElementBits >
            config_.riscvVectorLengthBits) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires riscv_vector_element_bits not to exceed "
            "riscv_vector_length_bits\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAClock.empty()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u has an empty rx_dma_clock\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAWidthBits == 0 ||
        config_.receiveDMAWidthBits % 32 != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires rx_dma_width_bits to be a positive "
            "multiple of 32\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (config_.receiveDMAQueueDepth == 0 ||
        config_.receiveDMAQueueDepth >
            MITTENS_BRIDGE_BURST_QUEUE_CAPACITY) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires rx_dma_queue_depth in [1, %u]\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(
                MITTENS_BRIDGE_BURST_QUEUE_CAPACITY));
    }

    if (config_.analogBackend != "native" &&
        config_.analogBackend != "crosssim") {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u has unsupported analog_backend '%s'; expected native or crosssim\n",
            static_cast<unsigned>(config_.tileId),
            config_.analogBackend.c_str());
    }
    if (config_.analogLinkClock.empty()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has an empty analog_link_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogComputeLatencyCycles == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires analog_compute_latency_cycles to be nonzero\n",
            static_cast<unsigned>(config_.tileId));
    }

    if (config_.analogArrayRows == 0) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has zero analog_array_rows\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.analogArrayColumns == 0) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has zero analog_array_columns\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.profileMode != "off" &&
        config_.profileMode != "summary" &&
        config_.profileMode != "trace") {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u has unsupported profile_mode '%s'; expected off, "
            "summary, or trace\n",
            static_cast<unsigned>(config_.tileId),
            config_.profileMode.c_str());
    }
    if (config_.profileMode != "off" &&
        config_.profileOutputDirectory.empty()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires profile_output_directory when profiling is "
            "enabled\n",
            static_cast<unsigned>(config_.tileId));
    }

    if (config_.verbosity < 0) {
        output_.fatal(CALL_INFO, -1, "tile %u has a negative verbosity\n",
                      static_cast<unsigned>(config_.tileId));
    }
}

bool Tile::managedLaunch() const
{
    return config_.launchMode == "managed";
}

void Tile::init(unsigned phase)
{
    if (state_ == LifecycleState::Finished) {
        output_.fatal(CALL_INFO, -1, "tile %u was initialized after finish()\n",
                      static_cast<unsigned>(config_.tileId));
    }

    state_ = LifecycleState::Initializing;

    if (network_ != nullptr) {
        network_->init(phase);
    }
    if (memoryInterface_ != nullptr) {
        memoryInterface_->init(phase);
    }

    output_.verbose(CALL_INFO, 3, 0, "tile %u init phase %u\n",
                    static_cast<unsigned>(config_.tileId), phase);
}

void Tile::complete(unsigned phase)
{
    if (memoryInterface_ != nullptr) {
        memoryInterface_->complete(phase);
    }
}

void Tile::setup()
{
    if (state_ == LifecycleState::Finished) {
        output_.fatal(CALL_INFO, -1, "tile %u entered setup() after finish()\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (managedLaunch()) {
        try {
            syncBridge_.create(config_.tileId);
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "failed to create synchronization bridge for tile %u: %s\n",
                static_cast<unsigned>(config_.tileId),
                error.what());
        }
    }

    if (network_ != nullptr) {
        network_->setup();

        try {
            bridge_.create(config_.tileId);
        } catch (const std::exception& error) {
            output_.fatal(CALL_INFO, -1,
                          "failed to create bridge for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    if (memoryInterface_ != nullptr) {
        memoryInterface_->setup();
    }
    if (analogDevice_ != nullptr) {
        try {
            analogBridge_.create(
                config_.tileId,
                config_.analogArrayCount,
                config_.analogArrayRows,
                config_.analogArrayColumns);
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "failed to create analog bridge for tile %u: %s\n",
                static_cast<unsigned>(config_.tileId),
                error.what());
        }
    }

    state_ = LifecycleState::Setup;
    output_.verbose(CALL_INFO, 2, 0, "tile %u setup complete\n",
                    static_cast<unsigned>(config_.tileId));

    if (managedLaunch()) {
        output_.verbose(CALL_INFO, 1, 0, "starting QEMU for tile %u\n",
                        static_cast<unsigned>(config_.tileId));

        try {
            qemu_.start({
                config_.tileId,
                config_.qemuPath,
                config_.elfPath,
                config_.memory,
                config_.riscvVectorEnabled,
                config_.riscvVectorLengthBits,
                config_.riscvVectorElementBits,
                config_.memoryBackend == "memhierarchy",
                config_.memoryInitializationBatching,
                config_.scratchpadEnabled,
                UINT64_C(0x90000000),
                config_.scratchpadBytes,
                syncBridge_.fileDescriptor(),
                bridge_.fileDescriptor(),
                analogBridge_.fileDescriptor(),
            });
        } catch (const std::exception& error) {
            output_.fatal(CALL_INFO, -1, "failed to start QEMU for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }

        state_ = LifecycleState::Running;
        output_.verbose(CALL_INFO, 2, 0, "QEMU tile %u started with PID %d\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<int>(qemu_.pid()));
        cpuSyncLink_->send(new SST::Event());
    }
}

void Tile::handleCpuSyncEvent(SST::Event* event)
{
    delete event;

    if (state_ != LifecycleState::Running) {
        return;
    }
    if (observeQemuExit()) {
        return;
    }

    if (pendingSyncEvent_.has_value()) {
        (void)processPendingSyncEvent();
        return;
    }

    grantAndCaptureQemu();
}

void Tile::handleReceiveDMAEvent(SST::Event* event)
{
    delete event;

    if (!bridge_.open()) {
        return;
    }
    refreshReceiveDMATransfers();

    auto transfer = receiveDMATransfersInFlight_.end();
    for (auto candidate = receiveDMATransfersInFlight_.begin();
         candidate != receiveDMATransfersInFlight_.end();
         ++candidate) {
        if (!candidate->completionObserved) {
            transfer = candidate;
            break;
        }
    }
    if (transfer == receiveDMATransfersInFlight_.end()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received an RX DMA timer without a transfer\n",
            static_cast<unsigned>(config_.tileId));
    }

    const std::uint64_t currentCycle =
        getCurrentSimTime(receiveDMAClockTimeBase_);
    if (currentCycle < transfer->completionCycle) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u observed RX DMA burst %u too early at cycle %llu\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(transfer->burstIndex),
            static_cast<unsigned long long>(currentCycle));
    }

    transfer->completionObserved = true;

    if (memoryInterface_ != nullptr && config_.memoryTileStride != 0) {
        const std::uint64_t lineSize = config_.memoryCacheLineSize;
        const std::uint64_t byteCount =
            static_cast<std::uint64_t>(transfer->wordCount) *
            sizeof(std::uint32_t);
        const std::uint64_t firstGuest =
            transfer->destination - (transfer->destination % lineSize);
        const std::uint64_t lastGuest =
            (transfer->destination + byteCount - 1) -
            ((transfer->destination + byteCount - 1) % lineSize);
        for (std::uint64_t guest = firstGuest;; guest += lineSize) {
            const std::uint64_t timing =
                static_cast<std::uint64_t>(config_.tileId) *
                    config_.memoryTileStride +
                (guest - config_.memoryGuestBase);
            auto* const request =
                new SST::Interfaces::StandardMem::FlushAddr(
                    timing, lineSize, true, 1);
            receiveDMAInvalidations_.emplace(
                request->getID(), transfer->burstIndex);
            ++transfer->invalidationLines;
            ++transfer->invalidationResponsesPending;
            memoryInterface_->send(request);
            if (guest == lastGuest) {
                break;
            }
        }
        return;
    }

    authorizeReceiveDMA(*transfer);
}

void Tile::authorizeReceiveDMA(ReceiveDMATransfer& transfer)
{
    if (!bridge_.authorizeReceiveDMA()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u could not authorize RX DMA burst %u\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(transfer.burstIndex));
    }

    transfer.authorized = true;
    ++receiveDMATransfers_;
    receiveDMAWords_ += transfer.wordCount;
    receiveDMAActiveCycles_ += transfer.serviceCycles;
    performanceProfile_.recordReceiveDMA(
        "complete",
        transfer.source,
        transfer.routeId,
        transfer.executionId,
        transfer.burstIndex,
        transfer.wordCount,
        transfer.scheduleTick,
        transfer.startCycle,
        transfer.completionCycle,
        getCurrentSimCycle(),
        transfer.serviceCycles,
        transfer.invalidationLines);

    output_.verbose(
        CALL_INFO,
        2,
        0,
        "tile %u completed RX DMA burst %u from tile %u "
        "(route=%u, words=%u, dma_cycle=%llu)\n",
        static_cast<unsigned>(config_.tileId),
        static_cast<unsigned>(transfer.burstIndex),
        static_cast<unsigned>(transfer.source),
        static_cast<unsigned>(transfer.routeId),
        static_cast<unsigned>(transfer.wordCount),
        static_cast<unsigned long long>(
            getCurrentSimTime(receiveDMAClockTimeBase_)));

    resumeReceiveWaitIfReady();
}

void Tile::grantAndCaptureQemu()
{
    if (state_ != LifecycleState::Running ||
        observeQemuExit()) {
        return;
    }

    try {
        currentGrantEpoch_ =
            syncBridge_.grant(config_.syncInstructionQuantum);
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u failed to grant synchronized QEMU execution: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }

    lastGrantInstruction_ = 0;
    lastGrantVectorInstruction_ = 0;
    ++synchronizationGrants_;
    captureQemuEvent();
}

void Tile::resumeAndCaptureQemu()
{
    if (!pendingSyncEvent_.has_value() ||
        state_ != LifecycleState::Running) {
        return;
    }

    const QemuSyncEvent event = *pendingSyncEvent_;
    completeWait();
    try {
        syncBridge_.resume(event);
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u failed to resume synchronized QEMU execution: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }

    pendingSyncEvent_.reset();
    transmitWaitArmed_ = false;
    receiveWaitArmed_ = false;
    captureQemuEvent();
}

void Tile::captureQemuEvent()
{
    while (state_ == LifecycleState::Running) {
        std::optional<QemuSyncEvent> event;
        try {
            event = syncBridge_.waitForEvent(kSyncWaitTimeout);
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u failed while waiting for synchronized QEMU: %s\n",
                static_cast<unsigned>(config_.tileId),
                error.what());
        }

        if (!event.has_value()) {
            if (observeQemuExit()) {
                return;
            }
            continue;
        }

        if (event->grantEpoch != currentGrantEpoch_ ||
            event->instructionsExecuted < lastGrantInstruction_ ||
            event->vectorInstructionsExecuted <
                lastGrantVectorInstruction_ ||
            event->vectorInstructionsExecuted >
                event->instructionsExecuted ||
            event->instructionsExecuted >
                config_.syncInstructionQuantum) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u received invalid fd 41 event "
                "(epoch=%llu, expected=%llu, executed=%llu, previous=%llu, vectors=%llu, previous_vectors=%llu, quantum=%llu)\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(event->grantEpoch),
                static_cast<unsigned long long>(currentGrantEpoch_),
                static_cast<unsigned long long>(
                    event->instructionsExecuted),
                static_cast<unsigned long long>(
                    lastGrantInstruction_),
                static_cast<unsigned long long>(
                    event->vectorInstructionsExecuted),
                static_cast<unsigned long long>(
                    lastGrantVectorInstruction_),
                static_cast<unsigned long long>(
                    config_.syncInstructionQuantum));
        }

        const std::uint64_t instructions =
            event->instructionsExecuted - lastGrantInstruction_;
        const std::uint64_t vectorInstructions =
            event->vectorInstructionsExecuted -
            lastGrantVectorInstruction_;
        if (instructions > UINT64_MAX - cpuRunInstructions_ ||
            vectorInstructions >
                UINT64_MAX - cpuRunVectorInstructions_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u CPU run accounting overflowed\n",
                static_cast<unsigned>(config_.tileId));
        }
        cpuRunInstructions_ += instructions;
        cpuRunVectorInstructions_ += vectorInstructions;
        const std::uint64_t nextCpuRunCycles = std::max(
            divideRoundUp(
                cpuRunInstructions_, config_.cpuIssueWidth),
            cpuRunVectorInstructions_);
        if (nextCpuRunCycles < cpuRunCycles_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u CPU run cycles went backwards\n",
                static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t instructionCycles =
            nextCpuRunCycles - cpuRunCycles_;
        cpuRunCycles_ = nextCpuRunCycles;
        lastGrantInstruction_ = event->instructionsExecuted;
        lastGrantVectorInstruction_ =
            event->vectorInstructionsExecuted;
        synchronizedInstructions_ += instructions;
        synchronizedVectorInstructions_ += vectorInstructions;
        synchronizedCpuCycles_ += instructionCycles;
        if (instructionCycles >
            UINT64_MAX - scratchpadTimingCycle_) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u scratchpad time overflowed\n",
                static_cast<unsigned>(config_.tileId));
        }
        scratchpadTimingCycle_ += instructionCycles;
        if (event->stopReason != MITTENS_SYNC_STOP_QUANTUM_END) {
            /*
             * A host quantum split is not an architectural boundary, so
             * retain partially filled scalar issue slots and vector issue
             * occupancy across it. A real fd-41 device/task/exit boundary
             * closes the current contiguous CPU interval.
             */
            cpuRunInstructions_ = 0;
            cpuRunVectorInstructions_ = 0;
            cpuRunCycles_ = 0;
        }
        ++synchronizationEvents_;
        if (event->stopReason < synchronizationStopCounts_.size()) {
            ++synchronizationStopCounts_[event->stopReason];
        }
        pendingSyncEvent_ = *event;

        output_.verbose(
            CALL_INFO,
            3,
            0,
            "tile %u fd 41 event %llu: %s after %llu instructions (%llu vector) in grant %llu\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(
                event->eventSequence),
            syncStopReasonName(event->stopReason),
            static_cast<unsigned long long>(
                event->instructionsExecuted),
            static_cast<unsigned long long>(
                event->vectorInstructionsExecuted),
            static_cast<unsigned long long>(
                event->grantEpoch));

        scheduleCpuSyncEvent(instructionCycles);
        return;
    }
}

bool Tile::processPendingSyncEvent()
{
    if (!pendingSyncEvent_.has_value()) {
        return true;
    }

    const std::uint32_t reason =
        pendingSyncEvent_->stopReason;
    if (waitIsProfiled(reason)) {
        beginWait(*pendingSyncEvent_);
    }
    const bool drainStores =
        reason == MITTENS_SYNC_STOP_NIC_TRANSMIT ||
        reason == MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT ||
        reason == MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT ||
        reason == MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
        reason == MITTENS_SYNC_STOP_ANALOG_WAIT ||
        reason == MITTENS_SYNC_STOP_TASK_FINISH ||
        reason == MITTENS_SYNC_STOP_GUEST_EXIT;
    if (drainStores && outstandingMemoryWrites_ != 0) {
        return false;
    }
    switch (reason) {
    case MITTENS_SYNC_STOP_QUANTUM_END:
        serviceBridge();
        pendingSyncEvent_.reset();
        if (observeQemuExit()) {
            return true;
        }
        grantAndCaptureQemu();
        return true;

    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
        serviceBridge();
        if (transmitReadyForGuest(*pendingSyncEvent_)) {
            resumeAndCaptureQemu();
            return true;
        }
        transmitWaitArmed_ = true;
        return false;

    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
        serviceBridge();
        if (transmitReadyForGuest(*pendingSyncEvent_)) {
            resumeAndCaptureQemu();
            return true;
        }
        transmitWaitArmed_ = true;
        return false;

    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
        serviceBridge();
        if (receiveReadyForGuest()) {
            resumeAndCaptureQemu();
            return true;
        }
        receiveWaitArmed_ = true;
        return false;

    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
        serviceBridge();
        registerReceiveDMA(*pendingSyncEvent_);
        scheduleReceiveDMABursts();
        resumeAndCaptureQemu();
        return true;

    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
        serviceAnalogBridge();
        if (pendingAnalogEventReady()) {
            resumeAndCaptureQemu();
            return true;
        }
        return false;

    case MITTENS_SYNC_STOP_TASK_START:
        activeTaskId_ = pendingSyncEvent_->taskId;
        activeTaskExecutionId_ = pendingSyncEvent_->executionId;
        recordTaskTrace(*pendingSyncEvent_);
        resumeAndCaptureQemu();
        return true;

    case MITTENS_SYNC_STOP_TASK_FINISH:
        recordTaskTrace(*pendingSyncEvent_);
        activeTaskId_.reset();
        activeTaskExecutionId_ = 0;
        resumeAndCaptureQemu();
        return true;

    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
        if ((pendingSyncEvent_->memoryFlags &
             MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD) != 0) {
            if (scratchpadTimingModel_ == nullptr ||
                pendingSyncEvent_->memoryAddress <
                    UINT64_C(0x90000000)) {
                output_.fatal(
                    CALL_INFO, -1,
                    "tile %u received a scratchpad access while the "
                    "scratchpad is disabled\n",
                    static_cast<unsigned>(config_.tileId));
            }
            if (!scratchpadAccessDelayScheduled_) {
                const ScratchpadSchedule schedule =
                    scratchpadTimingModel_->scheduleCPU(
                        scratchpadTimingCycle_,
                        pendingSyncEvent_->memoryAddress -
                            UINT64_C(0x90000000),
                        pendingSyncEvent_->memorySize,
                        (pendingSyncEvent_->memoryFlags &
                         MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0);
                scratchpadAccessDelayScheduled_ = true;
                scratchpadTimingCycle_ = schedule.completionCycle;
                scheduleCpuSyncEvent(schedule.serviceCycles);
                return false;
            }
            scratchpadAccessDelayScheduled_ = false;
            resumeAndCaptureQemu();
            return true;
        }
        if (memoryInterface_ == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u received a memory event without a "
                "memHierarchy backend\n",
                static_cast<unsigned>(config_.tileId));
        }
        if ((pendingSyncEvent_->memoryFlags &
             MITTENS_SYNC_MEMORY_FLAG_WRITE) == 0 &&
            memoryReadHasPendingWriteHazard(*pendingSyncEvent_)) {
            return false;
        }
        if (!blockingMemoryRequestId_.has_value()) {
            issueMemoryRequest(*pendingSyncEvent_);
        }
        return false;

    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT: {
        if (scratchpadTimingModel_ == nullptr ||
            pendingSyncEvent_->scratchpadDMAByteCount() == 0) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u received an invalid scratchpad DMA submit\n",
                static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t source =
            pendingSyncEvent_->scratchpadDMASource();
        const std::uint64_t destination =
            pendingSyncEvent_->scratchpadDMADestination();
        const bool sourceIsScratchpad =
            source >= UINT64_C(0x90000000) &&
            source - UINT64_C(0x90000000) < config_.scratchpadBytes;
        const bool destinationIsScratchpad =
            destination >= UINT64_C(0x90000000) &&
            destination - UINT64_C(0x90000000) < config_.scratchpadBytes;
        if (sourceIsScratchpad == destinationIsScratchpad) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u scratchpad DMA must have one scratchpad endpoint\n",
                static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t offset =
            (sourceIsScratchpad ? source : destination) -
            UINT64_C(0x90000000);
        const ScratchpadSchedule schedule =
            scratchpadTimingModel_->scheduleDMA(
                scratchpadTimingCycle_,
                offset,
                pendingSyncEvent_->scratchpadDMAByteCount(),
                destinationIsScratchpad);
        auto& execution = scratchpadDMACompletions_[
            pendingSyncEvent_->executionId];
        if (!execution.emplace(
                pendingSyncEvent_->scratchpadDMATokenId(),
                schedule.completionCycle).second) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u reused scratchpad DMA token %u for execution %llu\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned>(
                    pendingSyncEvent_->scratchpadDMATokenId()),
                static_cast<unsigned long long>(
                    pendingSyncEvent_->executionId));
        }
        resumeAndCaptureQemu();
        return true;
    }

    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT: {
        auto execution = scratchpadDMACompletions_.find(
            pendingSyncEvent_->executionId);
        if (execution == scratchpadDMACompletions_.end()) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u waited for an unknown scratchpad DMA execution\n",
                static_cast<unsigned>(config_.tileId));
        }
        auto token = execution->second.find(
            pendingSyncEvent_->scratchpadDMATokenId());
        if (token == execution->second.end()) {
            output_.fatal(
                CALL_INFO, -1,
                "tile %u waited for an unknown scratchpad DMA token\n",
                static_cast<unsigned>(config_.tileId));
        }
        if (!scratchpadWaitDelayScheduled_ &&
            token->second > scratchpadTimingCycle_) {
            scratchpadWaitDelayScheduled_ = true;
            scheduleCpuSyncEvent(
                token->second - scratchpadTimingCycle_);
            scratchpadTimingCycle_ = token->second;
            return false;
        }
        scratchpadWaitDelayScheduled_ = false;
        execution->second.erase(token);
        if (execution->second.empty()) {
            scratchpadDMACompletions_.erase(execution);
        }
        resumeAndCaptureQemu();
        return true;
    }

    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE: {
        if (!config_.memoryInitializationBatching ||
            pendingSyncEvent_->memorySize != 0 ||
            pendingSyncEvent_->memoryFlags !=
                MITTENS_SYNC_MEMORY_FLAG_NONE) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u received an invalid aggregate memory "
                "initialization event\n",
                static_cast<unsigned>(config_.tileId));
        }
        if (!memoryInitializationDelayScheduled_) {
            const std::uint64_t reads =
                pendingSyncEvent_->
                    memoryInitializationReadBytes();
            const std::uint64_t writes =
                pendingSyncEvent_->
                    memoryInitializationWriteBytes();
            if (reads > UINT64_MAX - writes) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u initialization memory byte count "
                    "overflowed\n",
                    static_cast<unsigned>(config_.tileId));
            }
            const std::uint64_t bytes = reads + writes;
            const std::uint64_t transferCycles = divideRoundUp(
                bytes,
                config_.memoryInitializationBytesPerCycle);
            if (transferCycles >
                UINT64_MAX -
                    config_.memoryInitializationLatencyCycles) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u initialization memory cycle count "
                    "overflowed\n",
                    static_cast<unsigned>(config_.tileId));
            }
            const std::uint64_t cycles =
                config_.memoryInitializationLatencyCycles +
                transferCycles;

            ++memoryInitializationHandshakes_;
            memoryInitializationAccesses_ +=
                pendingSyncEvent_->
                    memoryInitializationAccesses();
            memoryInitializationReadBytes_ += reads;
            memoryInitializationWriteBytes_ += writes;
            memoryInitializationCycles_ += cycles;
            memoryInitializationDelayScheduled_ = true;
            scheduleCpuSyncEvent(cycles);
            return false;
        }
        memoryInitializationDelayScheduled_ = false;
        resumeAndCaptureQemu();
        return true;
    }

    case MITTENS_SYNC_STOP_GUEST_EXIT: {
        /*
         * A final partial TX ring may not have needed a TX_WAIT. Snapshot it
         * into SST before QEMU is reaped so every accepted burst drains.
         */
        serviceBridge();
        QemuExitStatus status;
        try {
            status = qemu_.waitForExit();
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "failed to reap QEMU tile %u after its fd 41 guest-exit event: %s\n",
                static_cast<unsigned>(config_.tileId),
                error.what());
        }
        handleQemuExit(status);
        return true;
    }

    default:
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received unsupported fd 41 stop reason %u\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(reason));
    }
    return false;
}

bool Tile::memoryReadHasPendingWriteHazard(
    const QemuSyncEvent& event) const noexcept
{
    const std::uint64_t readBegin = event.memoryAddress;
    const std::uint64_t readEnd =
        readBegin > UINT64_MAX - event.memorySize
            ? UINT64_MAX
            : readBegin + event.memorySize;
    for (const auto& entry : pendingMemoryRequests_) {
        const PendingMemoryRequest& pending = entry.second;
        if (!pending.write) {
            continue;
        }
        const std::uint64_t writeEnd =
            pending.address > UINT64_MAX - pending.size
                ? UINT64_MAX
                : pending.address + pending.size;
        if (readBegin < writeEnd && pending.address < readEnd) {
            return true;
        }
    }
    return false;
}

void Tile::issueMemoryRequest(const QemuSyncEvent& event)
{
    if (event.memorySize == 0 ||
        event.memorySize > 16 ||
        (event.memoryFlags & ~MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received invalid memory request "
            "(address=0x%llx, size=%u, flags=0x%x)\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(event.memoryAddress),
            static_cast<unsigned>(event.memorySize),
            static_cast<unsigned>(event.memoryFlags));
    }

    const bool write =
        (event.memoryFlags & MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0;
    std::uint64_t timingAddress = event.memoryAddress;
    if (config_.memoryTileStride != 0) {
        if (event.memoryAddress < config_.memoryGuestBase ||
            event.memoryAddress - config_.memoryGuestBase >=
                config_.memoryTileStride ||
            config_.tileId >
                (UINT64_MAX - (event.memoryAddress -
                               config_.memoryGuestBase)) /
                    config_.memoryTileStride) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u cannot translate memory address 0x%llx "
                "with base 0x%llx and stride 0x%llx\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(event.memoryAddress),
                static_cast<unsigned long long>(config_.memoryGuestBase),
                static_cast<unsigned long long>(config_.memoryTileStride));
        }
        timingAddress =
            static_cast<std::uint64_t>(config_.tileId) *
                config_.memoryTileStride +
            (event.memoryAddress - config_.memoryGuestBase);
    }
    SST::Interfaces::StandardMem::Request* request = nullptr;
    if (write) {
        request = new SST::Interfaces::StandardMem::Write(
            timingAddress,
            event.memorySize,
            std::vector<std::uint8_t>(event.memorySize, 0));
        ++memoryWrites_;
    } else {
        request = new SST::Interfaces::StandardMem::Read(
            timingAddress,
            event.memorySize);
        ++memoryReads_;
    }

    const PendingMemoryRequest pending{
        getCurrentSimCycle(),
        event.memoryAddress,
        timingAddress,
        event.memoryProgramCounter(),
        event.memoryReturnAddress(),
        event.memorySize,
        write,
        activeTaskId_.value_or(UINT32_MAX),
        activeTaskExecutionId_,
        activeTaskId_.has_value() ? "task" : "runtime",
    };
    const auto inserted = pendingMemoryRequests_.emplace(
        request->getID(), pending);
    if (!inserted.second) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u reused pending memory request ID %llu\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(request->getID()));
    }
    ++memoryRequests_;
    performanceProfile_.recordMemory(
        "issue",
        request->getID(),
        event.memoryAddress,
        timingAddress,
        pending.programCounter,
        pending.returnAddress,
        event.memorySize,
        write,
        pending.taskId,
        pending.executionId,
        pending.phase.c_str(),
        pending.issueTick,
        pending.issueTick);
    memoryInterface_->send(request);

    if (!write) {
        blockingMemoryRequestId_ = request->getID();
        return;
    }

    ++outstandingMemoryWrites_;
    if (outstandingMemoryWrites_ >=
        config_.memoryStoreBufferEntries) {
        blockingMemoryRequestId_ = request->getID();
        return;
    }

    /*
     * QEMU performs the architectural store immediately after this resume.
     * The StandardMem write continues independently as a timing-only store
     * buffer entry.  Reads remain blocking because the bridge does not yet
     * carry register-dependency information.
     */
    resumeAndCaptureQemu();
}

void Tile::handleMemoryResponse(
    SST::Interfaces::StandardMem::Request* request)
{
    if (request != nullptr) {
        const auto invalidation =
            receiveDMAInvalidations_.find(request->getID());
        if (invalidation != receiveDMAInvalidations_.end()) {
            const std::uint32_t burstIndex = invalidation->second;
            receiveDMAInvalidations_.erase(invalidation);
            delete request;
            for (ReceiveDMATransfer& transfer :
                 receiveDMATransfersInFlight_) {
                if (transfer.burstIndex != burstIndex) {
                    continue;
                }
                if (transfer.invalidationResponsesPending == 0) {
                    output_.fatal(
                        CALL_INFO,
                        -1,
                        "tile %u received an extra DMA invalidation "
                        "response for burst %u\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<unsigned>(burstIndex));
                }
                --transfer.invalidationResponsesPending;
                if (transfer.invalidationResponsesPending == 0) {
                    authorizeReceiveDMA(transfer);
                }
                return;
            }
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u lost DMA burst %u before invalidation "
                "completed\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned>(burstIndex));
        }
    }

    if (request == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received a null memory response\n",
            static_cast<unsigned>(config_.tileId));
    }

    const auto pending = pendingMemoryRequests_.find(request->getID());
    if (pending == pendingMemoryRequests_.end()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received unexpected memory response %s\n",
            static_cast<unsigned>(config_.tileId),
            request->getString().c_str());
    }
    const PendingMemoryRequest metadata = pending->second;

    performanceProfile_.recordMemory(
        "response",
        request->getID(),
        metadata.address,
        metadata.timingAddress,
        metadata.programCounter,
        metadata.returnAddress,
        metadata.size,
        metadata.write,
        metadata.taskId,
        metadata.executionId,
        metadata.phase.c_str(),
        metadata.issueTick,
        getCurrentSimCycle());
    const bool blockingLoadCompleted =
        !metadata.write &&
        blockingMemoryRequestId_.has_value() &&
        *blockingMemoryRequestId_ == request->getID();
    if (metadata.write) {
        if (outstandingMemoryWrites_ == 0) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u completed a store with no outstanding stores\n",
                static_cast<unsigned>(config_.tileId));
        }
        --outstandingMemoryWrites_;
    }
    pendingMemoryRequests_.erase(pending);
    delete request;
    ++memoryResponses_;

    if (blockingLoadCompleted) {
        blockingMemoryRequestId_.reset();
        resumeAndCaptureQemu();
        return;
    }

    if (blockingMemoryRequestId_.has_value() &&
        pendingSyncEvent_.has_value() &&
        pendingSyncEvent_->stopReason ==
            MITTENS_SYNC_STOP_MEMORY_ACCESS &&
        (pendingSyncEvent_->memoryFlags &
         MITTENS_SYNC_MEMORY_FLAG_WRITE) != 0 &&
        outstandingMemoryWrites_ <
            config_.memoryStoreBufferEntries) {
        blockingMemoryRequestId_.reset();
        resumeAndCaptureQemu();
        return;
    }

    if (pendingSyncEvent_.has_value() &&
        outstandingMemoryWrites_ == 0) {
        (void)processPendingSyncEvent();
        return;
    }
    if (pendingSyncEvent_.has_value() &&
        pendingSyncEvent_->stopReason ==
            MITTENS_SYNC_STOP_MEMORY_ACCESS &&
        (pendingSyncEvent_->memoryFlags &
         MITTENS_SYNC_MEMORY_FLAG_WRITE) == 0 &&
        !memoryReadHasPendingWriteHazard(*pendingSyncEvent_)) {
        (void)processPendingSyncEvent();
    }
}

bool Tile::pendingAnalogEventReady() const
{
    if (!pendingSyncEvent_.has_value() ||
        !analogBridge_.open()) {
        return false;
    }

    const QemuSyncEvent& event = *pendingSyncEvent_;
    const AnalogBridgeToken token{
        event.analogArrayId,
        static_cast<std::uint32_t>(event.analogSequence),
    };
    const std::uint32_t slotState =
        analogBridge_.slotState(token);
    const bool waitForCompletion =
        (event.flags &
         MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION) != 0 ||
        event.stopReason == MITTENS_SYNC_STOP_ANALOG_WAIT;

    if (waitForCompletion) {
        return slotState == MITTENS_ANALOG_SLOT_COMPLETED;
    }
    return slotState == MITTENS_ANALOG_SLOT_ACCEPTED ||
           slotState == MITTENS_ANALOG_SLOT_COMPLETED;
}

bool Tile::observeQemuExit()
{
    if (state_ != LifecycleState::Running) {
        return state_ == LifecycleState::Exited;
    }

    std::optional<QemuExitStatus> status;
    try {
        status = qemu_.pollExit();
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "failed to monitor QEMU tile %u: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }
    if (!status.has_value()) {
        return false;
    }

    handleQemuExit(*status);
    return true;
}

void Tile::handleQemuExit(const QemuExitStatus& status)
{
    completeWait();
    state_ = LifecycleState::Exited;
    pendingSyncEvent_.reset();
    receiveWaitArmed_ = false;
    serviceBridge();

    output_.verbose(
        CALL_INFO,
        1,
        0,
        "QEMU tile %u exited with %s; fd 41 synchronized "
        "%llu instructions in %llu grants and %llu events\n",
        static_cast<unsigned>(config_.tileId),
        status.describe().c_str(),
        static_cast<unsigned long long>(
            synchronizedInstructions_),
        static_cast<unsigned long long>(
            synchronizationGrants_),
        static_cast<unsigned long long>(
            synchronizationEvents_));

    if (!status.success()) {
        QemuProcess::terminateAll();
        output_.fatal(
            CALL_INFO,
            -1,
            "QEMU tile %u failed with %s\n",
            static_cast<unsigned>(config_.tileId),
            status.describe().c_str());
    }

    signalExitedTileIfDrained();
}

void Tile::handleNetworkCompletionEvent(SST::Event* event)
{
    delete event;

    if (!bridge_.open() || network_ == nullptr) {
        return;
    }
    completeReadyNetworkReceives();
    refreshReceiveDMATransfers();
    serviceIncomingPackets();
    scheduleReceiveDMABursts();
    checkBridgeError();
    resumeReceiveWaitIfReady();
    signalExitedTileIfDrained();
}

void Tile::scheduleCpuSyncEvent(
    std::uint64_t instructionCycles)
{
    if (cpuSyncLink_ == nullptr ||
        state_ != LifecycleState::Running) {
        return;
    }
    if (instructionCycles >
        static_cast<std::uint64_t>(
            std::numeric_limits<SST::SimTime_t>::max())) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u cannot schedule %llu synchronized CPU cycles\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(instructionCycles));
    }

    cpuSyncLink_->send(
        static_cast<SST::SimTime_t>(instructionCycles),
        new SST::Event());
}

bool Tile::clockAnalog(SST::Cycle_t)
{
    if (analogDevice_ != nullptr && analogDevice_->requiresTick()) {
        analogDevice_->tick();
    }
    serviceAnalogTrace();
    serviceAnalogCompletions();
    if (pendingSyncEvent_.has_value() &&
        (pendingSyncEvent_->stopReason ==
             MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
         pendingSyncEvent_->stopReason ==
             MITTENS_SYNC_STOP_ANALOG_WAIT) &&
        pendingAnalogEventReady()) {
        resumeAndCaptureQemu();
    }

    if (analogDevice_ == nullptr || !analogDevice_->requiresTick() ||
        state_ == LifecycleState::Exited ||
        state_ == LifecycleState::Finished) {
        analogClockRegistered_ = false;
        return true;
    }
    return false;
}

bool Tile::handleNetworkSend(int)
{
    if (bridge_.open() && network_ != nullptr) {
        serviceOutgoingPackets();
        checkBridgeError();
        resumeTransmitWaitIfReady();
    }
    signalExitedTileIfDrained();
    return true;
}

bool Tile::handleNetworkReceive(int)
{
    if (bridge_.open() && network_ != nullptr) {
        refreshReceiveDMATransfers();
        serviceIncomingPackets();
        scheduleReceiveDMABursts();
        checkBridgeError();
        resumeReceiveWaitIfReady();
    }
    return true;
}

void Tile::ensureAnalogClockRegistered()
{
    if (analogDevice_ != nullptr && analogDevice_->requiresTick() &&
        !analogClockRegistered_) {
        reregisterClock(analogClockTimeBase_, analogClockHandler_);
        analogClockRegistered_ = true;
    }
}

void Tile::serviceBridge()
{
    if (bridge_.open() && network_ != nullptr) {
        checkBridgeError();
        refreshReceiveDMATransfers();
        serviceIncomingPackets();
        scheduleReceiveDMABursts();
        serviceOutgoingPackets();
        checkBridgeError();
    }

    serviceAnalogBridge();
}

void Tile::serviceAnalogBridge()
{
    if (!analogBridge_.open() || analogDevice_ == nullptr) {
        return;
    }

    try {
        checkAnalogBridgeError();

        bool accepted;
        do {
            accepted = false;
            for (std::uint32_t arrayId = 0;
                 arrayId < analogBridge_.arrayCount();
                 ++arrayId) {
                std::optional<AnalogBridgeSubmission> submission =
                    analogBridge_.nextSubmission(arrayId);
                if (!submission.has_value() ||
                    !analogDevice_->canSubmit(submission->command)) {
                    continue;
                }

                const std::size_t inputWordCount =
                    submission->inputWords.size();
                const std::uint64_t ticket = analogDevice_->submit(
                    submission->command,
                    std::move(submission->inputWords));
                if (submission->command.operation <
                    analogOperationCounts_.size()) {
                    ++analogOperationCounts_[
                        submission->command.operation];
                }
                analogInputWords_ += inputWordCount;
                const auto inserted = analogRequests_.emplace(
                    ticket, submission->token);
                if (!inserted.second) {
                    throw std::logic_error(
                        "duplicate analog device ticket");
                }
                analogBridge_.markAccepted(submission->token);
                accepted = true;
            }
        } while (accepted);

        serviceAnalogTrace();
        serviceAnalogCompletions();
        ensureAnalogClockRegistered();
        checkAnalogBridgeError();
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u analog bridge failure: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }
}

void Tile::serviceAnalogTrace()
{
    if (analogDevice_ == nullptr ||
        !performanceProfile_.traceEnabled()) {
        return;
    }

    while (true) {
        const std::optional<AnalogTraceEvent> event =
            analogDevice_->takeTraceEvent();
        if (!event.has_value()) {
            return;
        }

        const char* phase = "unknown";
        switch (event->phase) {
        case AnalogTracePhase::Submitted:
            phase = "submitted";
            break;
        case AnalogTracePhase::InputTransferStart:
            phase = "input-transfer-start";
            break;
        case AnalogTracePhase::InputTransferFinish:
            phase = "input-transfer-finish";
            break;
        case AnalogTracePhase::ComputeStart:
            phase = "compute-start";
            break;
        case AnalogTracePhase::ComputeFinish:
            phase = "compute-finish";
            break;
        case AnalogTracePhase::OutputTransferStart:
            phase = "output-transfer-start";
            break;
        case AnalogTracePhase::OutputTransferFinish:
            phase = "output-transfer-finish";
            break;
        case AnalogTracePhase::MoveOutputStart:
            phase = "move-output-start";
            break;
        case AnalogTracePhase::MoveOutputFinish:
            phase = "move-output-finish";
            break;
        case AnalogTracePhase::MoveInputStart:
            phase = "move-input-start";
            break;
        case AnalogTracePhase::MoveInputFinish:
            phase = "move-input-finish";
            break;
        case AnalogTracePhase::Complete:
            phase = "complete";
            break;
        }
        performanceProfile_.recordAnalog(
            phase,
            event->ticket,
            event->operation,
            event->arrayId,
            event->deviceCycle,
            getCurrentSimCycle());
    }
}

void Tile::serviceAnalogCompletions()
{
    if (!analogBridge_.open() || analogDevice_ == nullptr) {
        return;
    }

    while (analogDevice_->completionReady()) {
        std::optional<AnalogCompletion> completion =
            analogDevice_->takeCompletion();
        if (!completion.has_value()) {
            return;
        }

        const auto request = analogRequests_.find(completion->ticket);
        if (request == analogRequests_.end()) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u has no bridge request for analog ticket %llu\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(completion->ticket));
        }
        analogBridge_.complete(
            request->second,
            completion->response.status,
            completion->outputWords);
        analogOutputWords_ += completion->outputWords.size();
        analogRequests_.erase(request);
    }
}

void Tile::serviceOutgoingPackets()
{
    constexpr int kVirtualNetwork = 0;
    constexpr int kPacketBits = 32;

    while (true) {
        if (!pendingTransmitBurst_.has_value()) {
            pendingTransmitBurst_ = bridge_.popTransmitBurst();
            if (pendingTransmitBurst_.has_value()) {
                pendingTransmitBurstReadyTick_ =
                    getCurrentSimCycle();
            }
        }
        if (pendingTransmitBurst_.has_value()) {
            const MittensBridgeTxBurst& burst =
                *pendingTransmitBurst_;
            if (burst.destination >= config_.networkSize) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u attempted to send a burst to invalid "
                    "destination %u\n",
                    static_cast<unsigned>(config_.tileId),
                    static_cast<unsigned>(burst.destination));
            }
            if (burst.word_count == 0 ||
                burst.word_count >
                    MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "tile %u submitted invalid %u-word burst\n",
                    static_cast<unsigned>(config_.tileId),
                    static_cast<unsigned>(burst.word_count));
            }

            const int burstBits =
                static_cast<int>(burst.word_count) * kPacketBits;
            if (!network_->spaceToSend(
                    kVirtualNetwork, burstBits)) {
                const std::uint64_t blockTick =
                    getCurrentSimCycle();
                const std::vector<std::uint32_t> blockedPayload(
                    burst.words,
                    burst.words + burst.word_count);
                OutgoingFrame blockedFrame = outgoingFrame_;
                const PacketEvent::Metadata blockedMetadata =
                    describePacket(
                        blockedPayload,
                        burst.destination,
                        pendingTransmitBurstReadyTick_.value_or(
                            blockTick),
                        blockTick,
                        blockedFrame);
                beginTransmitBlock(
                    blockedMetadata,
                    burst.destination,
                    blockedMetadata.protocolWords != 0
                        ? "frame-header"
                        : (blockedMetadata.payloadWords != 0
                               ? "frame-payload"
                               : "raw"),
                    burst.word_count,
                    1U + bridge_.transmitBurstCount());
                return;
            }

            std::vector<std::uint32_t> payload(
                burst.words,
                burst.words + burst.word_count);
            const std::uint64_t injectionTick =
                getCurrentSimCycle();
            completeTransmitBlock();
            OutgoingFrame nextFrame = outgoingFrame_;
            PacketEvent::Metadata metadata = describePacket(
                payload,
                burst.destination,
                pendingTransmitBurstReadyTick_.value_or(
                    injectionTick),
                injectionTick,
                nextFrame);
            auto* request =
                new SST::Interfaces::SimpleNetwork::Request(
                    burst.destination,
                    config_.tileId,
                    burstBits,
                    true,
                    true,
                    new PacketEvent(
                        std::move(payload), metadata));
            if (!network_->send(request, kVirtualNetwork)) {
                delete request;
                return;
            }
            outgoingFrame_ = nextFrame;
            ++nextNetworkPacketId_;
            ++networkTransmitPackets_;
            networkTransmitWords_ += burst.word_count;
            const std::uint32_t hops =
                meshHops(config_.tileId, burst.destination);
            networkWordHops_ +=
                static_cast<std::uint64_t>(burst.word_count) * hops;
            networkEndpointQueueTicks_ +=
                injectionTick >= metadata.readyTick
                    ? injectionTick - metadata.readyTick
                    : 0;
            performanceProfile_.recordNetwork(
                "inject",
                metadata.packetId,
                config_.tileId,
                burst.destination,
                metadata.routeId,
                metadata.executionId,
                metadata.protocolWords != 0
                    ? "frame-header"
                    : (metadata.payloadWords != 0
                           ? "frame-payload"
                           : "raw"),
                burst.word_count,
                metadata.protocolWords,
                metadata.payloadWords,
                hops,
                metadata.readyTick,
                metadata.injectionTick,
                injectionTick);

            output_.verbose(
                CALL_INFO,
                2,
                0,
                "tile %u sent %u-word burst to tile %u\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned>(burst.word_count),
                static_cast<unsigned>(burst.destination));
            pendingTransmitBurst_.reset();
            pendingTransmitBurstReadyTick_.reset();
            continue;
        }

        if (!pendingTransmit_.has_value()) {
            pendingTransmit_ = bridge_.popTransmit();
            if (pendingTransmit_.has_value()) {
                pendingTransmitReadyTick_ =
                    getCurrentSimCycle();
            }
        }
        if (!pendingTransmit_.has_value()) {
            signalExitedTileIfDrained();
            return;
        }

        if (pendingTransmit_->destination >= config_.networkSize) {
            output_.fatal(CALL_INFO, -1,
                          "tile %u attempted to send to invalid destination %u\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(pendingTransmit_->destination));
        }

        if (!network_->spaceToSend(kVirtualNetwork, kPacketBits)) {
            const std::uint64_t blockTick = getCurrentSimCycle();
            const std::vector<std::uint32_t> blockedPayload{
                pendingTransmit_->payload};
            OutgoingFrame blockedFrame = outgoingFrame_;
            const PacketEvent::Metadata blockedMetadata =
                describePacket(
                    blockedPayload,
                    pendingTransmit_->destination,
                    pendingTransmitReadyTick_.value_or(blockTick),
                    blockTick,
                    blockedFrame);
            beginTransmitBlock(
                blockedMetadata,
                pendingTransmit_->destination,
                blockedMetadata.protocolWords != 0
                    ? "frame-header"
                    : (blockedMetadata.payloadWords != 0
                           ? "frame-payload"
                           : "raw"),
                1,
                1U + bridge_.transmitCount());
            return;
        }

        const std::uint64_t injectionTick = getCurrentSimCycle();
        completeTransmitBlock();
        std::vector<std::uint32_t> payload{
            pendingTransmit_->payload};
        OutgoingFrame nextFrame = outgoingFrame_;
        PacketEvent::Metadata metadata = describePacket(
            payload,
            pendingTransmit_->destination,
            pendingTransmitReadyTick_.value_or(injectionTick),
            injectionTick,
            nextFrame);
        auto* request = new SST::Interfaces::SimpleNetwork::Request(
            pendingTransmit_->destination,
            config_.tileId,
            kPacketBits,
            true,
            true,
            new PacketEvent(std::move(payload), metadata));

        if (!network_->send(request, kVirtualNetwork)) {
            delete request;
            return;
        }
        outgoingFrame_ = nextFrame;
        ++nextNetworkPacketId_;
        ++networkTransmitPackets_;
        ++networkTransmitWords_;
        const std::uint32_t hops = meshHops(
            config_.tileId, pendingTransmit_->destination);
        networkWordHops_ += hops;
        networkEndpointQueueTicks_ +=
            injectionTick >= metadata.readyTick
                ? injectionTick - metadata.readyTick
                : 0;
        performanceProfile_.recordNetwork(
            "inject",
            metadata.packetId,
            config_.tileId,
            pendingTransmit_->destination,
            metadata.routeId,
            metadata.executionId,
            metadata.protocolWords != 0
                ? "frame-header"
                : (metadata.payloadWords != 0
                       ? "frame-payload"
                       : "raw"),
            1,
            metadata.protocolWords,
            metadata.payloadWords,
            hops,
            metadata.readyTick,
            metadata.injectionTick,
            injectionTick);

        output_.verbose(CALL_INFO, 2, 0,
                        "tile %u sent payload 0x%08x to tile %u\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<unsigned>(pendingTransmit_->payload),
                        static_cast<unsigned>(pendingTransmit_->destination));
        pendingTransmit_.reset();
        pendingTransmitReadyTick_.reset();
    }
}

void Tile::beginTransmitBlock(
    const PacketEvent::Metadata& metadata,
    std::uint32_t destination,
    const char* kind,
    std::uint32_t words,
    std::uint32_t queueOccupancy)
{
    if (!activeTransmitBlock_.has_value()) {
        activeTransmitBlock_ = TransmitBlock{
            nextTransmitBlockSequence_++,
            getCurrentSimCycle(),
            metadata.routeId,
            metadata.executionId,
            destination,
            kind,
            words,
            1,
            queueOccupancy,
        };
        return;
    }

    TransmitBlock& block = *activeTransmitBlock_;
    if (block.routeId != metadata.routeId ||
        block.executionId != metadata.executionId ||
        block.destination != destination ||
        block.words != words ||
        block.kind != kind) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u changed the pending transmit while blocked\n",
            static_cast<unsigned>(config_.tileId));
    }
    ++block.retryCount;
    block.maximumQueueOccupancy =
        std::max(block.maximumQueueOccupancy, queueOccupancy);
}

void Tile::completeTransmitBlock()
{
    if (!activeTransmitBlock_.has_value()) {
        return;
    }
    const std::uint64_t finishTick = getCurrentSimCycle();
    const TransmitBlock block = std::move(*activeTransmitBlock_);
    activeTransmitBlock_.reset();
    const std::uint64_t duration =
        finishTick >= block.startTick
            ? finishTick - block.startTick
            : 0;
    transmitBlockedTicks_ += duration;
    ++transmitBlockedEvents_;
    transmitBlockedRetries_ += block.retryCount;
    transmitMaximumQueueOccupancy_ = std::max(
        transmitMaximumQueueOccupancy_,
        static_cast<std::uint64_t>(
            block.maximumQueueOccupancy));
    performanceProfile_.recordTransmitBlocked(
        block.eventSequence,
        block.routeId,
        block.executionId,
        block.destination,
        block.kind.c_str(),
        block.words,
        block.startTick,
        finishTick,
        block.retryCount,
        block.maximumQueueOccupancy);
}

void Tile::serviceIncomingPackets()
{
    constexpr int kVirtualNetwork = 0;

    while (bridge_.receiveHasBurstSpace() &&
           bridge_.receiveBurstCount() +
                   pendingNetworkReceives_.size() <
               config_.receiveDMAQueueDepth &&
           network_->requestToReceive(kVirtualNetwork)) {
        SST::Interfaces::SimpleNetwork::Request* request =
            network_->recv(kVirtualNetwork);
        if (request == nullptr) {
            return;
        }

        SST::Event* rawPayload = request->takePayload();
        auto* packet = dynamic_cast<PacketEvent*>(rawPayload);
        if (packet == nullptr) {
            delete rawPayload;
            delete request;
            output_.fatal(CALL_INFO, -1,
                          "tile %u received an invalid network payload\n",
                          static_cast<unsigned>(config_.tileId));
        }

        const auto source = request->src;
        const PacketEvent::Metadata metadata =
            packet->metadata();
        std::vector<std::uint32_t> payload =
            packet->payloads();
        delete packet;
        delete request;

        if (payload.empty() ||
            payload.size() >
                MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
            output_.fatal(CALL_INFO, -1,
                          "tile %u received an invalid %zu-word "
                          "network burst\n",
                          static_cast<unsigned>(config_.tileId),
                          payload.size());
        }
        const std::uint64_t headArrivalTick =
            getCurrentSimCycle();
        if (payload.size() >
            std::numeric_limits<std::uint64_t>::max() / 32) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u network burst size overflowed\n",
                static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t transferCycles = divideRoundUp(
            static_cast<std::uint64_t>(payload.size()) * 32,
            config_.meshLinkWidthBits);
        const std::uint64_t linkPeriod =
            networkClockTimeBase_.getFactor();
        if (transferCycles >
            std::numeric_limits<std::uint64_t>::max() /
                linkPeriod) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u network completion time overflowed\n",
                static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t transferTicks =
            transferCycles * linkPeriod;
        const std::uint64_t startTick = std::max(
            headArrivalTick,
            networkReceiveNextAvailableTick_);
        if (startTick >
            std::numeric_limits<std::uint64_t>::max() -
                transferTicks) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u network receive serialization overflowed\n",
                static_cast<unsigned>(config_.tileId));
        }
        networkReceiveNextAvailableTick_ =
            startTick + transferTicks;
        const std::uint64_t completionTick =
            networkReceiveNextAvailableTick_ - linkPeriod;
        const std::uint64_t completionDelayCycles =
            divideRoundUp(
                completionTick - headArrivalTick,
                linkPeriod);
        pendingNetworkReceives_.push_back(PendingNetworkReceive{
            static_cast<std::uint32_t>(source),
            metadata,
            std::move(payload),
            completionTick,
        });
        if (completionDelayCycles == 0) {
            completeReadyNetworkReceives();
        } else {
            networkCompletionLink_->send(
                completionDelayCycles,
                new SST::Event());
        }
    }
}

void Tile::completeReadyNetworkReceives()
{
    const std::uint64_t now = getCurrentSimCycle();
    auto receive = pendingNetworkReceives_.begin();
    while (receive != pendingNetworkReceives_.end()) {
        if (receive->completionTick > now) {
            ++receive;
            continue;
        }
        PendingNetworkReceive completed = std::move(*receive);
        receive = pendingNetworkReceives_.erase(receive);
        completeNetworkReceive(std::move(completed));
    }
}

void Tile::completeNetworkReceive(PendingNetworkReceive receive)
{
    const std::size_t words = receive.payload.size();
    if (!bridge_.pushReceiveBurst(receive.source, receive.payload)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u bridge burst RX queue rejected %zu words at "
            "packet completion\n",
            static_cast<unsigned>(config_.tileId),
            words);
    }

    ++networkReceivePackets_;
    networkReceiveWords_ += words;
    const std::uint64_t arrivalTick = getCurrentSimCycle();
    if (arrivalTick >= receive.metadata.injectionTick) {
        networkTransitTicks_ +=
            arrivalTick - receive.metadata.injectionTick;
    }
    if (receive.metadata.routeId != PacketEvent::InvalidRouteId &&
        receive.metadata.protocolWords != 0) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(receive.source) << 32U) |
            receive.metadata.routeId;
        incomingRouteExecutions_[key].push_back(
            receive.metadata.executionId);
    }
    performanceProfile_.recordNetwork(
        "arrive",
        receive.metadata.packetId,
        receive.source,
        config_.tileId,
        receive.metadata.routeId,
        receive.metadata.executionId,
        receive.metadata.protocolWords != 0
            ? "frame-header"
            : (receive.metadata.payloadWords != 0
                   ? "frame-payload"
                   : "raw"),
        static_cast<std::uint32_t>(words),
        receive.metadata.protocolWords,
        receive.metadata.payloadWords,
        meshHops(receive.source, config_.tileId),
        receive.metadata.readyTick,
        receive.metadata.injectionTick,
        arrivalTick);

    output_.verbose(
        CALL_INFO,
        2,
        0,
        "tile %u completed %zu-word network burst from tile %u\n",
        static_cast<unsigned>(config_.tileId),
        words,
        static_cast<unsigned>(receive.source));
}

void Tile::registerReceiveDMA(const QemuSyncEvent& event)
{
    if (event.receiveDMASource >= config_.networkSize ||
        event.receiveDMARouteId == UINT32_MAX ||
        event.receiveDMAWordCount == 0 ||
        (config_.memoryTileStride != 0 &&
         (event.memoryAddress < config_.memoryGuestBase ||
          event.memoryAddress - config_.memoryGuestBase >=
              config_.memoryTileStride ||
          static_cast<std::uint64_t>(event.receiveDMAWordCount) *
                  sizeof(std::uint32_t) >
              config_.memoryTileStride -
                  (event.memoryAddress - config_.memoryGuestBase)))) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received invalid RX DMA descriptor "
            "(source=%u, route=%u, words=%u)\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(event.receiveDMASource),
            static_cast<unsigned>(event.receiveDMARouteId),
            static_cast<unsigned>(event.receiveDMAWordCount));
    }

    const auto inserted = receiveDMADescriptors_.emplace(
        event.receiveDMASource,
        ReceiveDMADescriptor{
            event.receiveDMARouteId,
            takeRouteExecutionId(
                event.receiveDMASource,
                event.receiveDMARouteId),
            event.memoryAddress,
            event.receiveDMAWordCount,
            false,
        });
    if (!inserted.second) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received a duplicate RX DMA descriptor "
            "for source %u\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(event.receiveDMASource));
    }

    output_.verbose(
        CALL_INFO,
        2,
        0,
        "tile %u registered RX DMA from tile %u "
        "(route=%u, words=%u)\n",
        static_cast<unsigned>(config_.tileId),
        static_cast<unsigned>(event.receiveDMASource),
        static_cast<unsigned>(event.receiveDMARouteId),
        static_cast<unsigned>(event.receiveDMAWordCount));
}

bool Tile::receiveBurstScheduled(
    std::uint32_t burstIndex) const noexcept
{
    for (const ReceiveDMATransfer& transfer :
         receiveDMATransfersInFlight_) {
        if (transfer.burstIndex == burstIndex) {
            return true;
        }
    }
    return false;
}

void Tile::refreshReceiveDMATransfers()
{
    if (!bridge_.open()) {
        receiveDMATransfersInFlight_.clear();
        return;
    }

    const std::uint32_t readIndex =
        bridge_.receiveBurstReadIndex();
    while (!receiveDMATransfersInFlight_.empty() &&
           receiveDMATransfersInFlight_.front().burstIndex !=
               readIndex) {
        if (!receiveDMATransfersInFlight_.front().authorized) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u consumed RX DMA burst %u before authorization\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned>(
                    receiveDMATransfersInFlight_.front().burstIndex));
        }
        receiveDMATransfersInFlight_.pop_front();
    }
}

void Tile::scheduleReceiveDMABursts()
{
    if (!bridge_.open() || receiveDMALink_ == nullptr) {
        return;
    }

    refreshReceiveDMATransfers();
    const std::uint32_t burstCount =
        bridge_.receiveBurstCount();
    for (std::uint32_t offset = 0;
         offset < burstCount;
         ++offset) {
        const std::optional<ReceiveBurstInfo> burst =
            bridge_.peekReceiveBurst(offset);
        if (!burst.has_value()) {
            return;
        }
        if (receiveBurstScheduled(burst->absoluteIndex)) {
            continue;
        }

        auto descriptor =
            receiveDMADescriptors_.find(burst->source);
        if (descriptor == receiveDMADescriptors_.end()) {
            /*
             * This is a software-visible frame header. Its descriptor
             * does not exist until QEMU consumes and validates it.
             */
            return;
        }
        if (burst->wordCount >
            descriptor->second.remainingWords) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u RX DMA burst from tile %u has %u words, "
                "but route %u expects only %u more\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned>(burst->source),
                static_cast<unsigned>(burst->wordCount),
                static_cast<unsigned>(descriptor->second.routeId),
                static_cast<unsigned>(
                    descriptor->second.remainingWords));
        }

        const std::uint64_t currentCycle =
            getCurrentSimTime(receiveDMAClockTimeBase_);
        ReceiveDMASchedule timing;
        try {
            timing = receiveDMAEngine_.schedule(
                currentCycle,
                burst->wordCount,
                !descriptor->second.setupCharged);
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u cannot schedule RX DMA: %s\n",
                static_cast<unsigned>(config_.tileId),
                error.what());
        }

        const std::uint64_t destination =
            descriptor->second.destination;
        descriptor->second.destination +=
            static_cast<std::uint64_t>(burst->wordCount) *
            sizeof(std::uint32_t);
        descriptor->second.setupCharged = true;
        descriptor->second.remainingWords -= burst->wordCount;
        const std::uint32_t routeId =
            descriptor->second.routeId;
        const std::uint64_t executionId =
            descriptor->second.executionId;
        const std::uint64_t scheduleTick =
            getCurrentSimCycle();
        receiveDMATransfersInFlight_.push_back(
            ReceiveDMATransfer{
                burst->absoluteIndex,
                burst->source,
                routeId,
                executionId,
                destination,
                burst->wordCount,
                scheduleTick,
                timing.startCycle,
                timing.completionCycle,
                timing.serviceCycles,
                0,
                0,
                false,
                false,
            });
        performanceProfile_.recordReceiveDMA(
            "schedule",
            burst->source,
            routeId,
            executionId,
            burst->absoluteIndex,
            burst->wordCount,
            scheduleTick,
            timing.startCycle,
            timing.completionCycle,
            scheduleTick,
            timing.serviceCycles);
        if (descriptor->second.remainingWords == 0) {
            receiveDMADescriptors_.erase(descriptor);
        }

        receiveDMALink_->send(
            static_cast<SST::SimTime_t>(
                timing.completionCycle - currentCycle),
            new SST::Event());

        output_.verbose(
            CALL_INFO,
            3,
            0,
            "tile %u scheduled RX DMA burst %u from tile %u "
            "(route=%u, words=%u, start=%llu, complete=%llu)\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(burst->absoluteIndex),
            static_cast<unsigned>(burst->source),
            static_cast<unsigned>(routeId),
            static_cast<unsigned>(burst->wordCount),
            static_cast<unsigned long long>(timing.startCycle),
            static_cast<unsigned long long>(
                timing.completionCycle));
    }
}

bool Tile::receiveReadyForGuest() const noexcept
{
    if (!bridge_.open()) {
        return false;
    }
    if (bridge_.receiveHasWordData()) {
        return true;
    }

    const std::optional<ReceiveBurstInfo> burst =
        bridge_.peekReceiveBurst();
    if (!burst.has_value()) {
        return false;
    }
    if (receiveBurstScheduled(burst->absoluteIndex)) {
        return bridge_.receiveDMAAuthorizationAvailable();
    }
    if (receiveDMADescriptors_.find(burst->source) !=
        receiveDMADescriptors_.end()) {
        return false;
    }
    return true;
}

void Tile::checkBridgeError() const
{
    const std::uint32_t error = bridge_.protocolError();
    if (error != MITTENS_BRIDGE_ERROR_NONE) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u QEMU NIC reported bridge protocol error %u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(error));
    }
}

void Tile::checkAnalogBridgeError() const
{
    const std::uint32_t error = analogBridge_.protocolError();
    if (error != MITTENS_ANALOG_BRIDGE_ERROR_NONE) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u QEMU analog device reported bridge protocol error %u\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned>(error));
    }
}

bool Tile::outgoingPacketsIdle() const noexcept
{
    return !pendingTransmit_.has_value() &&
           !pendingTransmitBurst_.has_value();
}

bool Tile::transmitReadyForGuest(
    const QemuSyncEvent& event) const noexcept
{
    return (event.flags & MITTENS_SYNC_EVENT_FLAG_NIC_BURST) != 0
        ? bridge_.transmitBurstHasSpace()
        : bridge_.transmitHasSpace();
}

bool Tile::pendingTransmitWait() const noexcept
{
    return transmitWaitArmed_ &&
           pendingSyncEvent_.has_value() &&
           (pendingSyncEvent_->stopReason ==
                MITTENS_SYNC_STOP_NIC_TRANSMIT ||
            pendingSyncEvent_->stopReason ==
                MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT);
}

void Tile::resumeTransmitWaitIfReady()
{
    if (pendingTransmitWait() &&
        transmitReadyForGuest(*pendingSyncEvent_)) {
        resumeAndCaptureQemu();
    }
}

bool Tile::pendingReceiveWait() const noexcept
{
    return receiveWaitArmed_ &&
           pendingSyncEvent_.has_value() &&
           pendingSyncEvent_->stopReason ==
               MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT;
}

void Tile::resumeReceiveWaitIfReady()
{
    if (pendingReceiveWait() && receiveReadyForGuest()) {
        resumeAndCaptureQemu();
    }
}

void Tile::beginWait(const QemuSyncEvent& event)
{
    if (activeWaitStartTick_.has_value()) {
        if (activeWaitEventSequence_ == event.eventSequence &&
            activeWaitReason_ == event.stopReason) {
            return;
        }
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u began fd 41 wait event %llu before completing event "
            "%llu\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(event.eventSequence),
            static_cast<unsigned long long>(
                activeWaitEventSequence_));
    }
    activeWaitStartTick_ = getCurrentSimCycle();
    activeWaitReason_ = event.stopReason;
    activeWaitEventSequence_ = event.eventSequence;
}

void Tile::completeWait()
{
    if (!activeWaitStartTick_.has_value()) {
        return;
    }
    const std::uint64_t finishTick = getCurrentSimCycle();
    const std::uint64_t duration =
        finishTick >= *activeWaitStartTick_
            ? finishTick - *activeWaitStartTick_
            : 0;
    if (activeWaitReason_ < waitTicks_.size()) {
        waitTicks_[activeWaitReason_] += duration;
    }
    performanceProfile_.recordWait(
        *activeWaitStartTick_,
        finishTick,
        syncStopReasonName(activeWaitReason_),
        activeWaitEventSequence_);
    activeWaitStartTick_.reset();
    activeWaitReason_ = MITTENS_SYNC_STOP_NONE;
    activeWaitEventSequence_ = 0;
}

bool Tile::waitIsProfiled(std::uint32_t reason) const noexcept
{
    switch (reason) {
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
        return true;
    default:
        return false;
    }
}

std::uint32_t Tile::meshHops(
    std::uint32_t source,
    std::uint32_t destination) const noexcept
{
    if (source == destination) {
        return 0;
    }
    if (config_.meshWidth == 0 || config_.meshHeight == 0) {
        return 1;
    }
    const std::uint32_t sourceX = source % config_.meshWidth;
    const std::uint32_t sourceY = source / config_.meshWidth;
    const std::uint32_t destinationX =
        destination % config_.meshWidth;
    const std::uint32_t destinationY =
        destination / config_.meshWidth;
    const std::uint32_t horizontal =
        sourceX > destinationX
            ? sourceX - destinationX
            : destinationX - sourceX;
    const std::uint32_t vertical =
        sourceY > destinationY
            ? sourceY - destinationY
            : destinationY - sourceY;
    return horizontal + vertical;
}

PacketEvent::Metadata Tile::describePacket(
    const std::vector<std::uint32_t>& payload,
    std::uint32_t destination,
    std::uint64_t readyTick,
    std::uint64_t injectionTick,
    OutgoingFrame& nextFrame) const
{
    PacketEvent::Metadata metadata;
    metadata.packetId = nextNetworkPacketId_;
    metadata.readyTick = readyTick;
    metadata.injectionTick = injectionTick;

    if (!nextFrame.active &&
        payload.size() == kDeploymentFrameHeaderWords &&
        payload[0] == kDeploymentFrameMagic) {
        nextFrame.active = true;
        nextFrame.destination = destination;
        nextFrame.routeId = payload[1];
        nextFrame.executionId =
            static_cast<std::uint64_t>(payload[2]) |
            (static_cast<std::uint64_t>(payload[3]) << 32U);
        nextFrame.remainingPayloadWords = payload[4];
        metadata.routeId = nextFrame.routeId;
        metadata.executionId = nextFrame.executionId;
        metadata.protocolWords =
            static_cast<std::uint32_t>(payload.size());
        return metadata;
    }

    if (nextFrame.active &&
        nextFrame.destination == destination &&
        payload.size() <= nextFrame.remainingPayloadWords) {
        metadata.routeId = nextFrame.routeId;
        metadata.executionId = nextFrame.executionId;
        metadata.payloadWords =
            static_cast<std::uint32_t>(payload.size());
        nextFrame.remainingPayloadWords -= metadata.payloadWords;
        if (nextFrame.remainingPayloadWords == 0) {
            nextFrame = OutgoingFrame{};
        }
        return metadata;
    }

    metadata.payloadWords =
        static_cast<std::uint32_t>(payload.size());
    return metadata;
}

std::uint64_t Tile::takeRouteExecutionId(
    std::uint32_t source,
    std::uint32_t routeId) noexcept
{
    const std::uint64_t key =
        (static_cast<std::uint64_t>(source) << 32U) |
        routeId;
    auto found = incomingRouteExecutions_.find(key);
    if (found == incomingRouteExecutions_.end() ||
        found->second.empty()) {
        return 0;
    }
    const std::uint64_t executionId = found->second.front();
    found->second.pop_front();
    if (found->second.empty()) {
        incomingRouteExecutions_.erase(found);
    }
    return executionId;
}

void Tile::signalExitedTileIfDrained()
{
    if (state_ == LifecycleState::Exited &&
        !primaryEndSignaled_ &&
        outgoingPacketsIdle()) {
        primaryEndSignaled_ = true;
        primaryComponentOKToEndSim();
    }
}

void Tile::recordTaskTrace(const QemuSyncEvent& event)
{
    if (event.taskId == UINT32_MAX) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u received a task trace event without a task ID\n",
            static_cast<unsigned>(config_.tileId));
    }
    const char* const eventName =
        event.stopReason == MITTENS_SYNC_STOP_TASK_START
            ? "start"
            : "finish";
    if (!config_.taskTraceDirectory.empty()) {
        openTaskTrace();
        taskTraceStream_
            << getCurrentSimCycle() << ','
            << eventName << ','
            << config_.tileId << ','
            << event.taskId << ','
            << event.executionId << ','
            << synchronizedInstructions_ << ','
            << synchronizedCpuCycles_ << '\n';
        taskTraceStream_.flush();
        return;
    }
    output_.output(
        "MITTENS_TASK_TRACE sim_time_ticks=%llu event=%s "
        "tile=%u task=%u execution=%llu\n",
        static_cast<unsigned long long>(getCurrentSimCycle()),
        eventName,
        static_cast<unsigned>(config_.tileId),
        static_cast<unsigned>(event.taskId),
        static_cast<unsigned long long>(event.executionId));
}

void Tile::openTaskTrace()
{
    if (taskTraceStream_.is_open()) {
        return;
    }
    const std::string path =
        config_.taskTraceDirectory + "/tile-" +
        std::to_string(config_.tileId) + ".csv";
    taskTraceStream_.open(path, std::ios::out | std::ios::trunc);
    if (!taskTraceStream_.is_open()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u could not open task trace file %s\n",
            static_cast<unsigned>(config_.tileId),
            path.c_str());
    }
    taskTraceStream_
        << "sim_time_ticks,event,tile_id,task_id,execution_id,"
           "retired_instructions,cpu_cycles\n";
}

void Tile::reportProfile() const
{
    output_.verbose(
        CALL_INFO,
        1,
        0,
        "MITTENS_PROFILE tile=%u instructions=%llu vector_instructions=%llu cpu_cycles=%llu issue_width=%u grants=%llu events=%llu "
        "stop_quantum=%llu stop_nic_tx=%llu stop_nic_rx=%llu "
        "stop_nic_rx_dma_submit=%llu "
        "stop_analog_submit=%llu stop_analog_wait=%llu "
        "stop_task_start=%llu stop_task_finish=%llu "
        "stop_memory=%llu stop_memory_init=%llu "
        "memory_requests=%llu "
        "memory_responses=%llu memory_reads=%llu memory_writes=%llu "
        "memory_init_handshakes=%llu memory_init_accesses=%llu "
        "memory_init_read_bytes=%llu memory_init_write_bytes=%llu "
        "memory_init_cycles=%llu "
        "network_tx_packets=%llu network_tx_words=%llu "
        "network_rx_packets=%llu network_rx_words=%llu "
        "rx_dma_transfers=%llu rx_dma_words=%llu "
        "rx_dma_active_cycles=%llu "
        "analog_active_cycles=%llu analog_link_beats=%llu "
        "analog_set=%llu analog_load=%llu "
        "analog_compute=%llu analog_store=%llu analog_move=%llu "
        "analog_input_words=%llu analog_output_words=%llu\n",
        static_cast<unsigned>(config_.tileId),
        static_cast<unsigned long long>(synchronizedInstructions_),
        static_cast<unsigned long long>(
            synchronizedVectorInstructions_),
        static_cast<unsigned long long>(synchronizedCpuCycles_),
        static_cast<unsigned>(config_.cpuIssueWidth),
        static_cast<unsigned long long>(synchronizationGrants_),
        static_cast<unsigned long long>(synchronizationEvents_),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_QUANTUM_END]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_TRANSMIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[
                MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_SUBMIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_WAIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_START]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_TASK_FINISH]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_ACCESS]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[
                MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE]),
        static_cast<unsigned long long>(memoryRequests_),
        static_cast<unsigned long long>(memoryResponses_),
        static_cast<unsigned long long>(memoryReads_),
        static_cast<unsigned long long>(memoryWrites_),
        static_cast<unsigned long long>(
            memoryInitializationHandshakes_),
        static_cast<unsigned long long>(
            memoryInitializationAccesses_),
        static_cast<unsigned long long>(
            memoryInitializationReadBytes_),
        static_cast<unsigned long long>(
            memoryInitializationWriteBytes_),
        static_cast<unsigned long long>(
            memoryInitializationCycles_),
        static_cast<unsigned long long>(networkTransmitPackets_),
        static_cast<unsigned long long>(networkTransmitWords_),
        static_cast<unsigned long long>(networkReceivePackets_),
        static_cast<unsigned long long>(networkReceiveWords_),
        static_cast<unsigned long long>(receiveDMATransfers_),
        static_cast<unsigned long long>(receiveDMAWords_),
        static_cast<unsigned long long>(receiveDMAActiveCycles_),
        static_cast<unsigned long long>(
            analogDevice_ == nullptr
                ? 0
                : analogDevice_->elapsedCycles()),
        static_cast<unsigned long long>(
            analogDevice_ == nullptr
                ? 0
                : analogDevice_->linkBeats()),
        static_cast<unsigned long long>(
            analogOperationCounts_[MITTENS_ANALOG_OPERATION_SET_MATRIX]),
        static_cast<unsigned long long>(
            analogOperationCounts_[MITTENS_ANALOG_OPERATION_LOAD_VECTOR]),
        static_cast<unsigned long long>(
            analogOperationCounts_[MITTENS_ANALOG_OPERATION_COMPUTE]),
        static_cast<unsigned long long>(
            analogOperationCounts_[MITTENS_ANALOG_OPERATION_STORE_VECTOR]),
        static_cast<unsigned long long>(
            analogOperationCounts_[MITTENS_ANALOG_OPERATION_MOVE_VECTOR]),
        static_cast<unsigned long long>(analogInputWords_),
        static_cast<unsigned long long>(analogOutputWords_));

    const ScratchpadTimingStatistics scratchpad =
        scratchpadTimingModel_ == nullptr
            ? ScratchpadTimingStatistics{}
            : scratchpadTimingModel_->statistics();
    output_.verbose(
        CALL_INFO,
        1,
        0,
        "MITTENS_SCRATCHPAD_PROFILE tile=%u enabled=%u capacity_bytes=%llu "
        "cpu_requests=%llu dma_transfers=%llu dma_bytes=%llu "
        "bank_conflicts=%llu queue_cycles=%llu active_cycles=%llu "
        "stop_dma_submit=%llu stop_dma_wait=%llu\n",
        static_cast<unsigned>(config_.tileId),
        config_.scratchpadEnabled ? 1U : 0U,
        static_cast<unsigned long long>(config_.scratchpadBytes),
        static_cast<unsigned long long>(scratchpad.cpuRequests),
        static_cast<unsigned long long>(scratchpad.dmaTransfers),
        static_cast<unsigned long long>(scratchpad.dmaBytes),
        static_cast<unsigned long long>(scratchpad.bankConflicts),
        static_cast<unsigned long long>(scratchpad.queueCycles),
        static_cast<unsigned long long>(scratchpad.activeCycles),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[
                MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            synchronizationStopCounts_[
                MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT]));

    output_.verbose(
        CALL_INFO,
        1,
        0,
        "MITTENS_PROFILE_TIMING tile=%u network_word_hops=%llu "
        "network_transit_ticks=%llu network_endpoint_queue_ticks=%llu "
        "wait_nic_tx_ticks=%llu wait_nic_rx_ticks=%llu "
        "wait_rx_dma_submit_ticks=%llu wait_analog_submit_ticks=%llu "
        "wait_analog_completion_ticks=%llu wait_memory_ticks=%llu "
        "wait_memory_init_ticks=%llu "
        "wait_scratchpad_dma_submit_ticks=%llu "
        "wait_scratchpad_dma_wait_ticks=%llu\n",
        static_cast<unsigned>(config_.tileId),
        static_cast<unsigned long long>(networkWordHops_),
        static_cast<unsigned long long>(networkTransitTicks_),
        static_cast<unsigned long long>(
            networkEndpointQueueTicks_),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_NIC_TRANSMIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_ANALOG_SUBMIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_ANALOG_WAIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_MEMORY_ACCESS]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT]),
        static_cast<unsigned long long>(
            waitTicks_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT]));
}

void Tile::writePerformanceSummary()
{
    std::array<
        const char*,
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT + 1>
        waitReasonNames{};
    for (std::size_t index = 0;
         index < waitReasonNames.size();
         ++index) {
        waitReasonNames[index] =
            syncStopReasonName(static_cast<std::uint32_t>(index));
    }
    performanceProfile_.writeSummary(
        getCurrentSimCycle(),
        synchronizedInstructions_,
        synchronizedVectorInstructions_,
        synchronizedCpuCycles_,
        networkTransmitPackets_,
        networkTransmitWords_,
        networkWordHops_,
        networkTransitTicks_,
        networkEndpointQueueTicks_,
        analogDevice_ == nullptr
            ? 0
            : analogDevice_->elapsedCycles(),
        analogDevice_ == nullptr
            ? 0
            : analogDevice_->linkBeats(),
        receiveDMAActiveCycles_,
        transmitBlockedTicks_,
        transmitBlockedEvents_,
        transmitBlockedRetries_,
        transmitMaximumQueueOccupancy_,
        waitTicks_.data(),
        waitReasonNames.data(),
        waitTicks_.size());
}

void Tile::finish()
{
    if (state_ == LifecycleState::Finished) {
        return;
    }

    if (qemu_.running()) {
        output_.verbose(CALL_INFO, 1, 0,
                        "terminating QEMU tile %u during finish()\n",
                        static_cast<unsigned>(config_.tileId));
        qemu_.terminate();
    }

    if (network_ != nullptr) {
        network_->finish();
    }
    if (memoryInterface_ != nullptr) {
        memoryInterface_->finish();
    }

    completeWait();
    serviceAnalogTrace();
    reportProfile();
    try {
        writePerformanceSummary();
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u could not write performance profile: %s\n",
            static_cast<unsigned>(config_.tileId),
            error.what());
    }

    bridge_.close();
    analogBridge_.close();
    syncBridge_.close();

    state_ = LifecycleState::Finished;
    output_.verbose(CALL_INFO, 2, 0, "tile %u finished\n",
                    static_cast<unsigned>(config_.tileId));
}

void Tile::emergencyShutdown()
{
    qemu_.terminate();
    bridge_.close();
    analogBridge_.close();
    syncBridge_.close();
}

} // namespace Mittens
} // namespace SST
