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

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

#include "analog/analogDevice.h"
#include "qemuProcess.h"
#include "receiveDMAEngine.h"
#include "sharedAnalogMemoryBridge.h"
#include "sharedMemoryBridge.h"
#include "sharedSyncMemoryBridge.h"

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
        {"qemu_path", "Path to the QEMU system emulator", "qemu-system-riscv64"},
        {"elf", "Bare-metal ELF image loaded by QEMU", ""},
        {"memory", "Private QEMU RAM assigned to this tile", "16M"},
        {"launch_mode", "QEMU launch mode: disabled or managed", "disabled"},
        {"cpu_clock", "Clock defining the synchronized one-instruction CPU cycle", "1GHz"},
        {"sync_instruction_quantum", "Maximum instructions SST grants QEMU at once", "1000"},
        {"rx_dma_clock", "Clock for the tile-local NIC-to-scratchpad receive DMA engine", "1GHz"},
        {"rx_dma_width_bits", "Receive DMA transfer width; positive multiple of 32 bits", "256"},
        {"rx_dma_setup_cycles", "One-time setup cycles charged per receive descriptor", "8"},
        {"rx_dma_queue_depth", "Finite incoming burst queue depth, from 1 through 4", "4"},
        {"analog_array_count", "Simulation-wide number of analog arrays instantiated on every tile; zero disables analog", "0"},
        {"analog_array_rows", "Simulation-wide row count shared by every analog array", "100"},
        {"analog_array_columns", "Simulation-wide column count shared by every analog array", "100"},
        {"analog_backend", "Analog numerical backend: native or crosssim", "native"},
        {"crosssim_config", "Optional CrossSim JSON parameter file used by this tile's independent backend", ""},
        {"analog_link_clock", "Common clock for every array's independent bidirectional 256-bit link", "1GHz"},
        {"analog_compute_latency_cycles", "Analog compute latency in analog-link cycles", "100"},
        {"task_trace_directory", "Optional directory for per-tile task trace CSV files", ""},
        {"verbose", "Mittens diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS()

    SST_ELI_DOCUMENT_STATISTICS()

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Network interface connecting the tile to the SST mesh",
         "SST::Interfaces::SimpleNetwork"})

    Tile(SST::ComponentId_t id, SST::Params& params);

    void init(unsigned phase) override;
    void setup() override;
    void finish() override;
    void emergencyShutdown() override;

  private:
    struct Configuration {
        std::uint32_t tileId;
        std::uint32_t networkSize;
        std::string qemuPath;
        std::string elfPath;
        std::string memory;
        std::string launchMode;
        std::string cpuClock;
        std::uint64_t syncInstructionQuantum;
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
        std::uint32_t remainingWords;
        bool setupCharged;
    };

    struct ReceiveDMATransfer {
        std::uint32_t burstIndex;
        std::uint32_t source;
        std::uint32_t routeId;
        std::uint32_t wordCount;
        std::uint64_t completionCycle;
        std::uint64_t serviceCycles;
        bool authorized;
    };

    static Configuration readConfiguration(SST::Params& params);
    void validateConfiguration() const;
    bool managedLaunch() const;
    void handleCpuSyncEvent(SST::Event* event);
    void handleReceiveDMAEvent(SST::Event* event);
    void grantAndCaptureQemu();
    void resumeAndCaptureQemu();
    void captureQemuEvent();
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
    void serviceOutgoingPackets();
    void serviceIncomingPackets();
    void registerReceiveDMA(const QemuSyncEvent& event);
    void scheduleReceiveDMABursts();
    void refreshReceiveDMATransfers();
    bool receiveReadyForGuest() const noexcept;
    bool receiveBurstScheduled(std::uint32_t burstIndex) const noexcept;
    void checkBridgeError() const;
    void checkAnalogBridgeError() const;
    bool outgoingPacketsIdle() const noexcept;
    bool pendingReceiveWait() const noexcept;
    void resumeReceiveWaitIfReady();
    void signalExitedTileIfDrained();
    void recordTaskTrace(const QemuSyncEvent& event);
    void openTaskTrace();
    void reportProfile() const;

    Configuration config_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_;
    ReceiveDMAEngine receiveDMAEngine_;
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
    SST::TimeConverter receiveDMAClockTimeBase_;
    SST::Link* receiveDMALink_ = nullptr;
    std::optional<QemuSyncEvent> pendingSyncEvent_;
    std::uint64_t currentGrantEpoch_ = 0;
    std::uint64_t lastGrantInstruction_ = 0;
    std::uint64_t synchronizedInstructions_ = 0;
    std::uint64_t synchronizationGrants_ = 0;
    std::uint64_t synchronizationEvents_ = 0;
    std::array<std::uint64_t, MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT + 1>
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
    std::unordered_map<std::uint32_t, ReceiveDMADescriptor>
        receiveDMADescriptors_;
    std::deque<ReceiveDMATransfer> receiveDMATransfersInFlight_;
    std::optional<MittensBridgePacket> pendingTransmit_;
    std::optional<MittensBridgeTxBurst> pendingTransmitBurst_;
    bool receiveWaitArmed_ = false;
    bool primaryEndSignaled_ = false;
    std::ofstream taskTraceStream_;
    LifecycleState state_;
};

} // namespace Mittens
} // namespace SST

#endif
