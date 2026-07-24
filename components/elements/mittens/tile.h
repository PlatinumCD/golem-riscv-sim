#ifndef SST_MITTENS_TILE_H
#define SST_MITTENS_TILE_H

#include <cstdint>
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
        {"analog_array_count", "Simulation-wide number of analog arrays instantiated on every tile; zero disables analog", "0"},
        {"analog_array_rows", "Simulation-wide row count shared by every analog array", "100"},
        {"analog_array_columns", "Simulation-wide column count shared by every analog array", "100"},
        {"analog_backend", "Analog numerical backend: native or crosssim", "native"},
        {"crosssim_config", "Optional CrossSim JSON parameter file used by this tile's independent backend", ""},
        {"analog_link_clock", "Common clock for every array's independent bidirectional 256-bit link", "1GHz"},
        {"analog_compute_latency_cycles", "Analog compute latency in analog-link cycles", "100"},
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
        std::uint32_t analogArrayCount;
        std::uint32_t analogArrayRows;
        std::uint32_t analogArrayColumns;
        std::string analogBackend;
        std::string crossSimConfig;
        std::string analogLinkClock;
        std::uint64_t analogComputeLatencyCycles;
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

    static Configuration readConfiguration(SST::Params& params);
    void validateConfiguration() const;
    bool managedLaunch() const;
    void handleCpuSyncEvent(SST::Event* event);
    void grantAndCaptureQemu();
    void resumeAndCaptureQemu();
    void captureQemuEvent();
    bool processPendingSyncEvent();
    bool pendingAnalogEventReady() const;
    bool observeQemuExit();
    void handleQemuExit(const QemuExitStatus& status);
    void scheduleCpuSyncEvent(std::uint64_t instructionCycles);
    bool clockAnalog(SST::Cycle_t cycle);
    void ensureAnalogClockRegistered();
    void serviceBridge();
    void serviceAnalogBridge();
    void serviceAnalogCompletions();
    void serviceOutgoingPackets();
    void serviceIncomingPackets();
    void checkBridgeError() const;
    void checkAnalogBridgeError() const;
    bool outgoingPacketsIdle() const noexcept;

    Configuration config_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_;
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
    std::optional<QemuSyncEvent> pendingSyncEvent_;
    std::uint64_t currentGrantEpoch_ = 0;
    std::uint64_t lastGrantInstruction_ = 0;
    std::uint64_t synchronizedInstructions_ = 0;
    std::uint64_t synchronizationGrants_ = 0;
    std::uint64_t synchronizationEvents_ = 0;
    std::optional<MittensBridgePacket> pendingTransmit_;
    LifecycleState state_;
};

} // namespace Mittens
} // namespace SST

#endif
