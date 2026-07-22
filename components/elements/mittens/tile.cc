#include "sst_config.h"

#include "tile.h"

#include "packetEvent.h"

namespace SST {
namespace Mittens {

Tile::Configuration Tile::readConfiguration(SST::Params& params)
{
    return {
        params.find<std::uint32_t>("tile_id", 0),
        params.find<std::uint32_t>("network_size", 0),
        params.find<std::string>("qemu_path", "qemu-system-riscv64"),
        params.find<std::string>("elf", ""),
        params.find<std::string>("memory", "16M"),
        params.find<std::string>("launch_mode", "disabled"),
        params.find<std::string>("process_poll_frequency", "1MHz"),
        params.find<int>("verbose", 0),
    };
}

Tile::Tile(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    config_(readConfiguration(params)),
    output_("mittens: ", config_.verbosity, 0, SST::Output::STDOUT),
    network_(nullptr),
    state_(LifecycleState::Constructed)
{
    validateConfiguration();

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
    if (config_.networkSize != 0 && config_.tileId >= config_.networkSize) {
        output_.fatal(CALL_INFO, -1,
                      "tile ID %u is outside network_size %u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(config_.networkSize));
    }

    if (managedLaunch()) {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
        registerClock(config_.processPollFrequency,
                      new SST::Clock::Handler<Tile, &Tile::pollQemu>(this));
    }

    output_.verbose(
        CALL_INFO,
        1,
        0,
        "configured tile %u (qemu=%s, elf=%s, memory=%s, launch=%s, network=%s, network_size=%u)\n",
        static_cast<unsigned>(config_.tileId),
        config_.qemuPath.c_str(),
        config_.elfPath.empty() ? "<unset>" : config_.elfPath.c_str(),
        config_.memory.c_str(),
        config_.launchMode.c_str(),
        network_ == nullptr ? "detached" : "attached",
        static_cast<unsigned>(config_.networkSize));
}

void Tile::validateConfiguration() const
{
    if (config_.qemuPath.empty()) {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty qemu_path\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (config_.memory.empty()) {
        output_.fatal(CALL_INFO, -1, "tile %u has an empty memory setting\n",
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

    if (config_.processPollFrequency.empty()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has an empty process_poll_frequency\n",
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

    output_.verbose(CALL_INFO, 3, 0, "tile %u init phase %u\n",
                    static_cast<unsigned>(config_.tileId), phase);
}

void Tile::setup()
{
    if (state_ == LifecycleState::Finished) {
        output_.fatal(CALL_INFO, -1, "tile %u entered setup() after finish()\n",
                      static_cast<unsigned>(config_.tileId));
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
                bridge_.fileDescriptor(),
            });
        } catch (const std::exception& error) {
            output_.fatal(CALL_INFO, -1, "failed to start QEMU for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }

        state_ = LifecycleState::Running;
        output_.verbose(CALL_INFO, 2, 0, "QEMU tile %u started with PID %d\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<int>(qemu_.pid()));
    }
}

bool Tile::pollQemu(SST::Cycle_t)
{
    serviceBridge();

    if (state_ == LifecycleState::Exited) {
        return outgoingPacketsIdle();
    }
    if (state_ != LifecycleState::Running) {
        return state_ == LifecycleState::Finished;
    }

    std::optional<QemuExitStatus> status;
    try {
        status = qemu_.pollExit();
    } catch (const std::exception& error) {
        output_.fatal(CALL_INFO, -1, "failed to monitor QEMU tile %u: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    if (!status.has_value()) {
        return false;
    }

    state_ = LifecycleState::Exited;
    output_.verbose(CALL_INFO, 1, 0, "QEMU tile %u exited with %s\n",
                    static_cast<unsigned>(config_.tileId),
                    status->describe().c_str());

    if (!status->success()) {
        output_.fatal(CALL_INFO, -1, "QEMU tile %u failed with %s\n",
                      static_cast<unsigned>(config_.tileId),
                      status->describe().c_str());
    }

    primaryComponentOKToEndSim();
    serviceBridge();
    return outgoingPacketsIdle();
}

void Tile::serviceBridge()
{
    if (!bridge_.open() || network_ == nullptr) {
        return;
    }

    checkBridgeError();
    serviceIncomingPackets();
    serviceOutgoingPackets();
    checkBridgeError();
}

void Tile::serviceOutgoingPackets()
{
    constexpr int kVirtualNetwork = 0;
    constexpr int kPacketBits = 64;

    while (true) {
        if (!pendingTransmit_.has_value()) {
            pendingTransmit_ = bridge_.popTransmit();
        }
        if (!pendingTransmit_.has_value()) {
            return;
        }

        if (pendingTransmit_->destination >= config_.networkSize) {
            output_.fatal(CALL_INFO, -1,
                          "tile %u attempted to send to invalid destination %u\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(pendingTransmit_->destination));
        }

        if (!network_->spaceToSend(kVirtualNetwork, kPacketBits)) {
            return;
        }

        auto* request = new SST::Interfaces::SimpleNetwork::Request(
            pendingTransmit_->destination,
            config_.tileId,
            kPacketBits,
            true,
            true,
            new PacketEvent(pendingTransmit_->payload));

        if (!network_->send(request, kVirtualNetwork)) {
            delete request;
            return;
        }

        output_.verbose(CALL_INFO, 2, 0,
                        "tile %u sent payload 0x%08x to tile %u\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<unsigned>(pendingTransmit_->payload),
                        static_cast<unsigned>(pendingTransmit_->destination));
        pendingTransmit_.reset();
    }
}

void Tile::serviceIncomingPackets()
{
    constexpr int kVirtualNetwork = 0;

    while (bridge_.receiveHasSpace() &&
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

        const std::uint32_t payload = packet->payload();
        const auto source = request->src;
        delete packet;
        delete request;

        if (!bridge_.pushReceive(payload)) {
            output_.fatal(CALL_INFO, -1,
                          "tile %u bridge RX queue became full unexpectedly\n",
                          static_cast<unsigned>(config_.tileId));
        }

        output_.verbose(CALL_INFO, 2, 0,
                        "tile %u received payload 0x%08x from tile %lld\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<unsigned>(payload),
                        static_cast<long long>(source));
    }
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

bool Tile::outgoingPacketsIdle() const noexcept
{
    return !pendingTransmit_.has_value();
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

    bridge_.close();

    state_ = LifecycleState::Finished;
    output_.verbose(CALL_INFO, 2, 0, "tile %u finished\n",
                    static_cast<unsigned>(config_.tileId));
}

void Tile::emergencyShutdown()
{
    qemu_.terminate();
    bridge_.close();
}

} // namespace Mittens
} // namespace SST
