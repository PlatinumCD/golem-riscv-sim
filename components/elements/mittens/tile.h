#ifndef SST_MITTENS_TILE_H
#define SST_MITTENS_TILE_H

#include <cstdint>
#include <optional>
#include <string>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>

#include "qemuProcess.h"
#include "sharedMemoryBridge.h"

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
        {"process_poll_frequency", "Frequency used to poll managed QEMU", "1MHz"},
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
        std::string processPollFrequency;
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
    bool pollQemu(SST::Cycle_t cycle);
    void serviceBridge();
    void serviceOutgoingPackets();
    void serviceIncomingPackets();
    void checkBridgeError() const;
    bool outgoingPacketsIdle() const noexcept;

    Configuration config_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_;
    QemuProcess qemu_;
    SharedMemoryBridge bridge_;
    std::optional<MittensBridgePacket> pendingTransmit_;
    LifecycleState state_;
};

} // namespace Mittens
} // namespace SST

#endif
