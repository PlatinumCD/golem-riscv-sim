#ifndef SST_MITTENS_TILE_H
#define SST_MITTENS_TILE_H

#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

#include "analog/analogDevice.h"
#include "packetEvent.h"
#include "performanceProfile.h"
#include "qemuProcess.h"
#include "receiveDMAEngine.h"
#include "sharedAnalogMemoryBridge.h"
#include "sharedMemoryBridge.h"
#include "sharedSyncMemoryBridge.h"
#include "scratchpad/scratchpadTimingModel.h"

namespace SST {
namespace Mittens {

class Tile : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        Tile,
        "mittens",
        "tile",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "QEMU-backed bare-metal RISC-V tile",
        COMPONENT_CATEGORY_PROCESSOR)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Linear mesh tile identifier", "0"},
        {"network_size", "Number of valid destination tile IDs", "0"},
        {"mesh_width", "Physical mesh width used for hop accounting; zero means unspecified", "0"},
        {"mesh_height", "Physical mesh height used for hop accounting; zero means unspecified", "0"},
        {"mesh_link_clock", "Clock defining one physical mesh transfer cycle", "1GHz"},
        {"mesh_link_width_bits", "Physical mesh link width in bits per transfer cycle", "32"},
        {"network_packet_words", "Maximum 32-bit words in one SST network request. This value must not exceed the endpoint buffer capacity", "16"},
        {"network_tail_delivery", "The network interface delivers a request only after its tail flit arrives", "false"},
        {"qemu_path", "Path to the QEMU system emulator", "qemu-system-riscv64"},
        {"elf", "Bare-metal ELF image loaded by QEMU", ""},
        {"memory", "Private QEMU RAM assigned to this tile", "16M"},
        {"memory_backend", "Data-memory timing backend: native or memhierarchy", "native"},
        {"memory_guest_base", "Guest physical base used for tile-namespaced timing addresses; zero preserves guest addresses", "0"},
        {"memory_tile_stride", "Per-tile timing-address stride; zero preserves guest addresses", "0"},
        {"memory_cache_line_size", "Cache line size used for receive-DMA invalidation", "64"},
        {"memory_load_queue_entries", "Timed load-queue capacity for grouped nonblocking loads", "8"},
        {"memory_store_buffer_entries", "Timed CPU store-buffer capacity; one preserves blocking behavior", "1"},
        {"memory_init_batching", "Aggregate pre-runtime data accesses into one fd 41 initialization handshake", "false"},
        {"memory_access_batching", "Batch fd 41 transport while replaying each runtime access through MemHierarchy", "false"},
        {"memory_access_batch_records", "Maximum ordered memory records in one fd 41 batch", "16"},
        {"memory_init_bytes_per_cycle", "Aggregate initialization bandwidth in bytes per CPU cycle", "32"},
        {"memory_init_latency_cycles", "One-time aggregate initialization latency in CPU cycles", "2"},
        {"scratchpad_enabled", "Enable the private noncoherent tile scratchpad", "false"},
        {"scratchpad_bytes", "Private scratchpad capacity in bytes", "262144"},
        {"scratchpad_banks", "Number of scratchpad banks", "8"},
        {"scratchpad_read_ports", "Read ports per scratchpad bank", "1"},
        {"scratchpad_write_ports", "Write ports per scratchpad bank", "1"},
        {"scratchpad_access_width_bits", "Scratchpad port width in bits", "256"},
        {"scratchpad_latency_cycles", "Scratchpad access latency in cycles", "1"},
        {"scratchpad_dma_bytes_per_cycle", "Scratchpad DMA width in bytes per cycle", "32"},
        {"scratchpad_dma_setup_cycles", "Scratchpad DMA setup latency in cycles", "8"},
        {"launch_mode", "QEMU launch mode: disabled or managed", "disabled"},
        {"cpu_clock", "Clock defining the synchronized CPU issue cycle", "1GHz"},
        {"cpu_issue_width", "Scalar front-end issue width: 1, 2, or 4 instructions per cycle; vector issue remains one per cycle", "1"},
        {"sync_instruction_quantum", "Maximum instructions SST grants QEMU at once", "1000"},
        {"riscv_vector_enabled", "Enable the standard RISC-V V extension in QEMU", "true"},
        {"riscv_vector_length_bits", "QEMU RISC-V vector register length (VLEN), a power of two from 128 through 1024 bits", "256"},
        {"riscv_vector_element_bits", "QEMU maximum RISC-V vector element width (ELEN), a power of two from 8 through 64 bits", "64"},
        {"rx_dma_clock", "Clock for the tile-local NIC-to-scratchpad receive DMA engine", "1GHz"},
        {"rx_dma_width_bits", "Receive DMA transfer width; positive multiple of 32 bits", "256"},
        {"rx_dma_setup_cycles", "One-time setup cycles charged per receive descriptor", "8"},
        {"rx_dma_queue_depth", "Finite incoming burst queue depth, from 1 through 4", "4"},
        {"analog_array_count", "Simulation-wide number of analog arrays instantiated on every tile; zero disables analog", "0"},
        {"analog_array_rows", "Simulation-wide row count shared by every analog array", "100"},
        {"analog_array_columns", "Simulation-wide column count shared by every analog array", "100"},
        {"analog_backend", "Analog numerical backend: native or crosssim", "native"},
        {"crosssim_config", "Optional CrossSim JSON parameter file used by this tile's independent backend", ""},
        {"analog_link_clock", "Clock for the tile-wide shared bidirectional 256-bit analog link", "1GHz"},
        {"analog_compute_latency_cycles", "Analog compute latency in analog-link cycles", "100"},
        {"profile_mode", "Performance profiling mode: off, summary, or trace", "off"},
        {"profile_output_directory", "Directory for per-tile performance profile files", ""},
        {"task_trace_directory", "Optional directory for per-tile task trace CSV files", ""},
        {"verbose", "Mittens diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS()

    SST_ELI_DOCUMENT_STATISTICS()

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Network interface connecting the tile to the SST mesh",
         "SST::Interfaces::SimpleNetwork"},
        {"memoryIF", "Optional StandardMem interface connecting this tile to a private L1",
         "SST::Interfaces::StandardMem"})

    Tile(SST::ComponentId_t id, SST::Params& params);

    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void setup() override;
    void finish() override;
    void emergencyShutdown() override;

  private:
    struct Configuration {
        std::uint32_t tileId;
        std::uint32_t networkSize;
        std::uint32_t meshWidth;
        std::uint32_t meshHeight;
        std::string meshLinkClock;
        std::uint32_t meshLinkWidthBits;
        std::uint32_t networkPacketWords;
        bool networkTailDelivery;
        std::string qemuPath;
        std::string elfPath;
        std::string memory;
        std::string memoryBackend;
        std::uint64_t memoryGuestBase;
        std::uint64_t memoryTileStride;
        std::uint32_t memoryCacheLineSize;
        std::uint32_t memoryLoadQueueEntries;
        std::uint32_t memoryStoreBufferEntries;
        bool memoryInitializationBatching;
        bool memoryAccessBatching;
        std::uint32_t memoryAccessBatchRecords;
        std::uint32_t memoryInitializationBytesPerCycle;
        std::uint64_t memoryInitializationLatencyCycles;
        bool scratchpadEnabled;
        std::uint64_t scratchpadBytes;
        std::uint32_t scratchpadBanks;
        std::uint32_t scratchpadReadPorts;
        std::uint32_t scratchpadWritePorts;
        std::uint32_t scratchpadAccessWidthBits;
        std::uint64_t scratchpadLatencyCycles;
        std::uint32_t scratchpadDMABytesPerCycle;
        std::uint64_t scratchpadDMASetupCycles;
        std::string launchMode;
        std::string cpuClock;
        std::uint32_t cpuIssueWidth;
        std::uint64_t syncInstructionQuantum;
        bool riscvVectorEnabled;
        std::uint32_t riscvVectorLengthBits;
        std::uint32_t riscvVectorElementBits;
        std::string receiveDMAClock;
        std::uint32_t receiveDMAWidthBits;
        std::uint64_t receiveDMASetupCycles;
        std::uint32_t receiveDMAQueueDepth;
        std::uint32_t analogArrayCount;
        std::uint32_t analogArrayRows;
        std::uint32_t analogArrayColumns;
        std::string analogBackend;
        std::string crossSimConfig;
        std::string analogLinkClock;
        std::uint64_t analogComputeLatencyCycles;
        std::string profileMode;
        std::string profileOutputDirectory;
        std::string taskTraceDirectory;
        int verbosity;
    };

    enum class LifecycleState {
        Constructed,
        Initializing,
        Setup,
        Running,
        Exited,
        Finished,
    };

    struct ReceiveDMADescriptor {
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint64_t destination;
        std::uint32_t remainingWords;
        bool setupCharged;
    };

    struct ReceiveDMATransfer {
        std::uint32_t burstIndex;
        std::uint32_t source;
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint64_t destination;
        std::uint32_t wordCount;
        std::uint64_t scheduleTick;
        std::uint64_t startCycle;
        std::uint64_t completionCycle;
        std::uint64_t serviceCycles;
        std::uint32_t invalidationLines;
        std::uint32_t invalidationResponsesPending;
        bool completionObserved;
        bool authorized;
    };

    struct PendingNetworkReceive {
        std::uint32_t source;
        PacketEvent::Metadata metadata;
        std::vector<std::uint32_t> payload;
        std::uint64_t completionTick;
    };

    struct IncomingFrameAssembly {
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint32_t expectedWords;
        std::vector<std::uint32_t> payload;
    };

    struct ReadyReceiveBurst {
        std::uint32_t source;
        std::vector<std::uint32_t> payload;
    };

    struct OutgoingFrame {
        bool active = false;
        std::uint32_t destination = UINT32_MAX;
        std::uint32_t routeId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::uint32_t remainingPayloadWords = 0;
    };

    struct TransmitBlock {
        std::uint64_t eventSequence = 0;
        std::uint64_t startTick = 0;
        std::uint32_t routeId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::uint32_t destination = UINT32_MAX;
        std::string kind;
        std::uint32_t words = 0;
        std::uint64_t retryCount = 0;
        std::uint32_t maximumQueueOccupancy = 0;
    };

    struct PendingMemoryRequest {
        std::uint64_t issueTick = 0;
        std::uint64_t address = 0;
        std::uint64_t timingAddress = 0;
        std::uint64_t programCounter = 0;
        std::uint64_t returnAddress = 0;
        std::uint32_t size = 0;
        bool write = false;
        std::uint32_t taskId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::string phase = "idle";
    };

    static Configuration readConfiguration(SST::Params& params);
    void validateConfiguration() const;
    bool managedLaunch() const;
    void handleCpuSyncEvent(SST::Event* event);
    void handleReceiveDMAEvent(SST::Event* event);
    void handleNetworkCompletionEvent(SST::Event* event);
    void handleMemoryResponse(
        SST::Interfaces::StandardMem::Request* request);
    void authorizeReceiveDMA(ReceiveDMATransfer& transfer);
    void grantAndCaptureQemu();
    void resumeAndCaptureQemu();
    void captureQemuEvent();
    std::uint64_t accountCpuTo(
        const QemuSyncEvent& event,
        bool architecturalBoundary);
    void beginMemoryBatch(const QemuSyncEvent& event);
    void scheduleNextMemoryBatchStep();
    void advanceMemoryBatchAccess();
    void advanceMemoryBatchGroup();
    bool processPendingSyncEvent();
    bool pendingAnalogEventReady() const;
    bool observeQemuExit();
    void handleQemuExit(const QemuExitStatus& status);
    void scheduleCpuSyncEvent(std::uint64_t instructionCycles);
    bool clockAnalog(SST::Cycle_t cycle);
    bool handleNetworkSend(int virtualNetwork);
    bool handleNetworkReceive(int virtualNetwork);
    void ensureAnalogClockRegistered();
    void serviceBridge();
    void serviceAnalogBridge();
    void serviceAnalogCompletions();
    void serviceAnalogTrace();
    void serviceOutgoingPackets();
    void beginTransmitBlock(
        const PacketEvent::Metadata& metadata,
        std::uint32_t destination,
        const char* kind,
        std::uint32_t words,
        std::uint32_t queueOccupancy);
    void completeTransmitBlock();
    void serviceIncomingPackets();
    void completeReadyNetworkReceives();
    void completeNetworkReceive(PendingNetworkReceive receive);
    void flushReadyReceiveBursts();
    void registerReceiveDMA(const QemuSyncEvent& event);
    void scheduleReceiveDMABursts();
    void refreshReceiveDMATransfers();
    bool receiveReadyForGuest() const noexcept;
    bool receiveBurstScheduled(std::uint32_t burstIndex) const noexcept;
    void checkBridgeError() const;
    void checkAnalogBridgeError() const;
    bool outgoingPacketsIdle() const noexcept;
    bool transmitReadyForGuest(
        const QemuSyncEvent& event) const noexcept;
    bool pendingTransmitWait() const noexcept;
    void resumeTransmitWaitIfReady();
    bool pendingReceiveWait() const noexcept;
    void resumeReceiveWaitIfReady();
    void signalExitedTileIfDrained();
    void recordTaskTrace(const QemuSyncEvent& event);
    void openTaskTrace();
    void beginWait(const QemuSyncEvent& event);
    void completeWait();
    bool waitIsProfiled(std::uint32_t reason) const noexcept;
    std::uint32_t meshHops(
        std::uint32_t source, std::uint32_t destination) const noexcept;
    PacketEvent::Metadata describePacket(
        const std::vector<std::uint32_t>& payload,
        std::uint32_t destination,
        std::uint64_t readyTick,
        std::uint64_t injectionTick,
        OutgoingFrame& nextFrame) const;
    std::uint64_t takeRouteExecutionId(
        std::uint32_t source, std::uint32_t routeId) noexcept;
    void writePerformanceSummary();
    void reportProfile() const;
    void issueMemoryRequest(const QemuSyncEvent& event);
    SST::Interfaces::StandardMem::Request::id_t sendMemoryRequest(
        const QemuSyncEvent& event);
    bool memoryReadHasPendingWriteHazard(
        const QemuSyncEvent& event) const noexcept;

    Configuration config_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_;
    SST::Interfaces::StandardMem* memoryInterface_;
    SST::TimeConverter memoryClockTimeBase_;
    ReceiveDMAEngine receiveDMAEngine_;
    std::unique_ptr<ScratchpadTimingModel> scratchpadTimingModel_;
    PerformanceProfile performanceProfile_;
    QemuProcess qemu_;
    SharedSyncMemoryBridge syncBridge_;
    SharedMemoryBridge bridge_;
    SharedAnalogMemoryBridge analogBridge_;
    std::unique_ptr<AnalogDevice> analogDevice_;
    std::unordered_map<std::uint64_t, AnalogBridgeToken>
        analogRequests_;
    SST::TimeConverter analogClockTimeBase_;
    SST::Clock::HandlerBase* analogClockHandler_ = nullptr;
    bool analogClockRegistered_ = false;
    SST::Link* cpuSyncLink_ = nullptr;
    SST::TimeConverter networkClockTimeBase_;
    SST::Link* networkCompletionLink_ = nullptr;
    SST::TimeConverter receiveDMAClockTimeBase_;
    SST::Link* receiveDMALink_ = nullptr;
    std::optional<QemuSyncEvent> pendingSyncEvent_;
    std::uint64_t currentGrantEpoch_ = 0;
    std::uint64_t lastGrantInstruction_ = 0;
    std::uint64_t lastGrantVectorInstruction_ = 0;
    std::uint64_t synchronizedInstructions_ = 0;
    std::uint64_t synchronizedVectorInstructions_ = 0;
    std::uint64_t synchronizedCpuCycles_ = 0;
    std::uint64_t cpuRunInstructions_ = 0;
    std::uint64_t cpuRunVectorInstructions_ = 0;
    std::uint64_t cpuRunCycles_ = 0;
    std::uint64_t synchronizationGrants_ = 0;
    std::uint64_t synchronizationEvents_ = 0;
    std::array<
        std::uint64_t,
        MITTENS_SYNC_STOP_MEMORY_FENCE + 1>
        synchronizationStopCounts_{};
    std::array<std::uint64_t, MITTENS_ANALOG_OPERATION_MOVE_VECTOR + 1>
        analogOperationCounts_{};
    std::uint64_t analogInputWords_ = 0;
    std::uint64_t analogOutputWords_ = 0;
    std::uint64_t networkTransmitPackets_ = 0;
    std::uint64_t networkTransmitWords_ = 0;
    std::uint64_t networkReceivePackets_ = 0;
    std::uint64_t networkReceiveWords_ = 0;
    std::uint64_t receiveDMATransfers_ = 0;
    std::uint64_t receiveDMAWords_ = 0;
    std::uint64_t receiveDMAActiveCycles_ = 0;
    std::uint64_t networkWordHops_ = 0;
    std::uint64_t networkTransitTicks_ = 0;
    std::uint64_t networkEndpointQueueTicks_ = 0;
    std::uint64_t networkReceiveNextAvailableTick_ = 0;
    std::optional<TransmitBlock> activeTransmitBlock_;
    std::uint64_t nextTransmitBlockSequence_ = 1;
    std::uint64_t transmitBlockedTicks_ = 0;
    std::uint64_t transmitBlockedEvents_ = 0;
    std::uint64_t transmitBlockedRetries_ = 0;
    std::uint64_t transmitMaximumQueueOccupancy_ = 0;
    std::uint64_t nextNetworkPacketId_ = 1;
    std::optional<std::uint64_t> pendingTransmitReadyTick_;
    std::optional<std::uint64_t> pendingTransmitBurstReadyTick_;
    OutgoingFrame outgoingFrame_;
    std::unordered_map<std::uint64_t, std::deque<std::uint64_t>>
        incomingRouteExecutions_;
    std::optional<std::uint64_t> activeWaitStartTick_;
    std::uint32_t activeWaitReason_ = MITTENS_SYNC_STOP_NONE;
    std::uint64_t activeWaitEventSequence_ = 0;
    std::array<
        std::uint64_t,
        MITTENS_SYNC_STOP_MEMORY_FENCE + 1>
        waitTicks_{};
    std::unordered_map<
        std::uint64_t,
        std::unordered_map<std::uint32_t, std::uint64_t>>
        scratchpadDMACompletions_;
    bool scratchpadAccessDelayScheduled_ = false;
    bool scratchpadWaitDelayScheduled_ = false;
    std::uint64_t scratchpadTimingCycle_ = 0;
    std::unordered_map<
        SST::Interfaces::StandardMem::Request::id_t,
        PendingMemoryRequest> pendingMemoryRequests_;
    std::optional<SST::Interfaces::StandardMem::Request::id_t>
        blockingMemoryRequestId_;
    std::optional<QemuSyncEvent> memoryBatchEnvelope_;
    std::vector<MittensSyncMemoryAccess> memoryBatchRecords_;
    std::size_t memoryBatchIndex_ = 0;
    std::size_t memoryBatchGroupEndIndex_ = 0;
    std::unordered_set<SST::Interfaces::StandardMem::Request::id_t>
        memoryBatchGroupRequestIds_;
    std::uint32_t outstandingMemoryWrites_ = 0;
    std::uint32_t outstandingMemoryReads_ = 0;
    std::uint64_t maximumOutstandingMemoryRequests_ = 0;
    std::uint64_t maximumOutstandingMemoryReads_ = 0;
    std::uint64_t maximumStoreBufferOccupancy_ = 0;
    std::uint64_t memoryStoreBufferFullEvents_ = 0;
    std::uint64_t vectorMemoryRequestGroups_ = 0;
    std::uint64_t vectorMemoryGroupRequests_ = 0;
    std::uint64_t scalarMemoryRequestGroups_ = 0;
    std::uint64_t scalarMemoryGroupRequests_ = 0;
    std::optional<std::uint32_t> activeTaskId_;
    std::uint64_t activeTaskExecutionId_ = 0;
    std::uint64_t memoryRequests_ = 0;
    std::uint64_t memoryResponses_ = 0;
    std::uint64_t memoryReads_ = 0;
    std::uint64_t memoryWrites_ = 0;
    bool memoryInitializationDelayScheduled_ = false;
    std::uint64_t memoryInitializationHandshakes_ = 0;
    std::uint64_t memoryInitializationAccesses_ = 0;
    std::uint64_t memoryInitializationReadBytes_ = 0;
    std::uint64_t memoryInitializationWriteBytes_ = 0;
    std::uint64_t memoryInitializationCycles_ = 0;
    std::unordered_map<std::uint32_t, ReceiveDMADescriptor>
        receiveDMADescriptors_;
    std::unordered_map<
        SST::Interfaces::StandardMem::Request::id_t,
        std::uint32_t> receiveDMAInvalidations_;
    std::deque<PendingNetworkReceive> pendingNetworkReceives_;
    std::unordered_map<std::uint32_t, IncomingFrameAssembly>
        incomingFrameAssemblies_;
    std::deque<ReadyReceiveBurst> readyReceiveBursts_;
    std::deque<ReceiveDMATransfer> receiveDMATransfersInFlight_;
    std::optional<MittensBridgePacket> pendingTransmit_;
    std::optional<MittensBridgeTxBurst> pendingTransmitBurst_;
    std::uint32_t pendingTransmitBurstOffset_ = 0;
    bool transmitWaitArmed_ = false;
    bool receiveWaitArmed_ = false;
    bool primaryEndSignaled_ = false;
    std::ofstream taskTraceStream_;
    LifecycleState state_;
};

} // namespace Mittens
} // namespace SST

#endif
