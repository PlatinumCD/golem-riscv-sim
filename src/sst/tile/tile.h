#ifndef SST_MITTENS_TILE_H
#define SST_MITTENS_TILE_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

#include "../analog/analogController.h"
#include "../memory/memoryAccessController.h"
#include "../memory/globalDMAClient.h"
#include "../memory/scratchpadBootImage.h"
#include "../synchronization/initializationBarrierClient.h"
#include "../execution/uniqueFileDescriptor.h"
#include "../configuration/tileConfiguration.h"
#include "../memory/addressRegion.h"
#include "../execution/clockDomain.h"
#include "../execution/cpuExecutionController.h"
#include <mittens/MemoryMap.h>
#include "../synchronization/epochBarrierEvent.h"
#include "../synchronization/memoryInitializationBarrierEvent.h"
#include "../network/packetEvent.h"
#include "../profiling/performanceProfile.h"
#include "../profiling/taskTrace.h"
#include "../profiling/tileMeasurements.h"
#include "../execution/qemuProcess.h"
#include "../network/rx/rxController.h"
#include "../network/tx/txController.h"
#include "../memory/globalDMAEvent.h"
#include "../bridge/sharedAnalogMemoryBridge.h"
#include "../bridge/sharedMemoryBridge.h"
#include "../bridge/sharedSyncMemoryBridge.h"
#include "../memory/scratchpad/scratchpadTimingModel.h"

namespace SST
{
namespace Mittens
{

class Tile : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(Tile, "mittens", "tile", SST_ELI_ELEMENT_VERSION(0, 1, 0),
                               "QEMU-backed bare-metal RISC-V tile", COMPONENT_CATEGORY_PROCESSOR)

#define MITTENS_ELI_PARAM(type, field, name, value, category, help, documented)                    \
    {name, help, documented},
    SST_ELI_DOCUMENT_PARAMS(MITTENS_TILE_PARAMETERS(MITTENS_ELI_PARAM))
#undef MITTENS_ELI_PARAM

    SST_ELI_DOCUMENT_PORTS({"globalDMA",
                            "Bidirectional global RAM DMA controller link",
                            {"mittens.GlobalDMAEvent"}},
                           {"memoryInitBarrier",
                            "Bidirectional modeled memory-initialization barrier link",
                            {"mittens.MemoryInitializationBarrierEvent"}},
                           {"epochBarrier",
                            "Bidirectional modeled deployment epoch barrier link",
                            {"mittens.EpochBarrierEvent"}})

    SST_ELI_DOCUMENT_STATISTICS({"scratchpad_service_cycles",
                                 "Tile-local scratchpad bank and DMA service cycles", "cycles", 1},
                                {"scratchpad_read_service_cycles",
                                 "Tile-local scratchpad read service cycles", "cycles", 1},
                                {"scratchpad_write_service_cycles",
                                 "Tile-local scratchpad write service cycles", "cycles", 1})

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({"networkIF",
                                         "Network interface connecting the tile to the SST mesh",
                                         "SST::Interfaces::SimpleNetwork"})

    Tile(SST::ComponentId_t id, SST::Params& params);
    ~Tile() override;

    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void setup() override;
    void finish() override;
    void emergencyShutdown() override;

  private:
    using Configuration = TileConfiguration;
    AddressRegion scratchpadRegion() const noexcept
    {
        return {MITTENS_SCRATCHPAD_BASE, config_.scratchpadBytes};
    }
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return Timing::Clock<Timing::Cpu>(cpuClockTimeBase_.getFactor());
    }
    Timing::Clock<Timing::Network> networkDomain() const
    {
        return Timing::Clock<Timing::Network>(networkClockTimeBase_.getFactor());
    }

    enum class LifecycleState
    {
        Constructed,
        Initializing,
        Setup,
        Running,
        Exited,
        Finished,
    };

    bool managedLaunch() const;
    void handleCpuSyncEvent(SST::Event* event);
    void handleRuntimeQemuReadySetDispatch(SST::Event* event);
    void handleReceiveDMAEvent(SST::Event* event);
    void handleTransmitDMAEvent(SST::Event* event);
    void handleGlobalDMAEvent(SST::Event* event);
    void submitBootSegment();
    bool advanceScratchpadBoot();
    void handleMemoryInitializationBarrierEvent(SST::Event* event);
    void handleEpochBarrierEvent(SST::Event* event);
    void handleNetworkCompletionEvent(SST::Event* event);
    void handleAnalogWakeEvent(SST::Event* event);

    bool observeQemuExit();
    void handleQemuExit(const QemuExitStatus& status);

    bool handleNetworkSend(int virtualNetwork);
    bool handleNetworkReceive(int virtualNetwork);

    void serviceBridge();

    void serviceOutgoingPackets();
    void checkBridgeError() const;
    void checkSyncBridgeError() const;

    bool outgoingPacketsIdle() const noexcept;

    void resumeTransmitWaitIfReady();

    void resumeReceiveWaitIfReady();
    void signalExitedTileIfDrained();
    void maybeProgressWatchdog();
    void recordProgressSnapshot(const char* kind);

    std::uint32_t meshHops(std::uint32_t source, std::uint32_t destination) const noexcept;
    TileMeasurementSnapshot measurementSnapshot() const;

    CpuDeviceResult executeCpuNetwork(const CpuNetworkAction& action);

    CpuDeviceResult executeCpuTask(const CpuTaskAction& action);

    void executeCpuExit();

    Configuration config_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_;
    PerformanceProfile performanceProfile_;
    TaskTrace taskTrace_;
    std::string resolvedConfigurationPath_;
    SST::Statistics::Statistic<std::uint64_t>* scratchpadServiceStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* scratchpadReadServiceStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* scratchpadWriteServiceStatistic_ = nullptr;
    QemuProcess qemu_;
    SharedSyncMemoryBridge syncBridge_;
    SharedMemoryBridge bridge_;
    std::unique_ptr<MemoryAccessController> memoryAccess_;
    std::unique_ptr<RxController> rx_;
    std::unique_ptr<TxController> tx_;
    SST::TimeConverter analogClockTimeBase_;
    SST::Link* analogWakeLink_ = nullptr;
    SST::Link* cpuSyncLink_ = nullptr;
    SST::Link* runtimeQemuReadySetLink_ = nullptr;
    SST::TimeConverter cpuClockTimeBase_;
    SST::TimeConverter networkClockTimeBase_;
    SST::Link* networkCompletionLink_ = nullptr;
    SST::Link* receiveDMALink_ = nullptr;
    SST::Link* transmitDMALink_ = nullptr;
    std::vector<SST::Link*> transmitDMAStreamLinks_;
    SST::Link* globalDMALink_ = nullptr;
    SST::Link* memoryInitializationBarrierLink_ = nullptr;
    SST::Link* epochBarrierLink_ = nullptr;
    UniqueFileDescriptor globalRAMFileDescriptor_;
    // QEMU has the functional image, but receives no execution grant until
    // every boot segment has passed through shared RAM and the SPM write port.
    std::optional<ScratchpadBootImage> bootImage_;
    std::size_t bootSegment_ = 0;
    std::uint64_t bootGlobalOffset_ = 0;
    std::optional<std::uint64_t> bootWriteCompletion_;
    bool bootReadPending_ = false;
    std::unique_ptr<AnalogController> analog_;
    std::unique_ptr<GlobalDMAClient> globalDMA_;
    std::unique_ptr<InitializationBarrierClient> barriers_;
    std::chrono::steady_clock::time_point lastProgressWallTime_{};
    std::chrono::steady_clock::time_point lastRetirementWallTime_{};
    std::chrono::steady_clock::time_point lastProgressSnapshotWallTime_{};
    std::uint64_t observedDeploymentProgressEpoch_ = 0;
    std::uint64_t observedMemoryInitializationExecutionProgressEpoch_ = 0;
    bool progressWatchdogInitialized_ = false;
    bool progressWatchdogReported_ = false;
    bool primaryEndSignaled_ = false;
    LifecycleState state_;
    // Declared last: capture destruction precedes every borrowed endpoint,
    // including constructor unwinding (the destructor body is not run then).
    std::unique_ptr<CpuExecutionController> cpu_;
    template <class F> void deviceNotification(F&& action)
    {
        try
        {
            action();
        }
        catch (const std::exception& error)
        {
            QemuProcess::terminateAll();
            output_.fatal(CALL_INFO, -1, "tile %u device notification failed: %s\n", config_.tileId,
                          error.what());
        }
    }
};

} // namespace Mittens
} // namespace SST

#endif
