#include "sst_config.h"

#include "tile.h"

#include "analog/crossSimAnalogBackend.h"
#include "analog/nativeAnalogBackend.h"
#include "packetEvent.h"

#include <chrono>
#include <exception>
#include <limits>

namespace SST {
namespace Mittens {

namespace {

constexpr auto kSyncWaitTimeout = std::chrono::milliseconds(50);

const char* syncStopReasonName(std::uint32_t reason)
{
    switch (reason) {
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
        params.find<std::string>("qemu_path", "qemu-system-riscv64"),
        params.find<std::string>("elf", ""),
        params.find<std::string>("memory", "16M"),
        params.find<std::string>("launch_mode", "disabled"),
        params.find<std::string>("cpu_clock", "1GHz"),
        params.find<std::uint64_t>("sync_instruction_quantum", 1000),
        params.find<std::uint32_t>("analog_array_count", 0),
        params.find<std::uint32_t>("analog_array_rows", 100),
        params.find<std::uint32_t>("analog_array_columns", 100),
        params.find<std::string>("analog_backend", "native"),
        params.find<std::string>("crosssim_config", ""),
        params.find<std::string>("analog_link_clock", "1GHz"),
        params.find<std::uint64_t>("analog_compute_latency_cycles", 100),
        params.find<int>("verbose", 0),
    };
    return configuration;
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
        "configured tile %u (qemu=%s, elf=%s, memory=%s, launch=%s, cpu_clock=%s, sync_quantum=%llu, network=%s, network_size=%u)\n",
        static_cast<unsigned>(config_.tileId),
        config_.qemuPath.c_str(),
        config_.elfPath.empty() ? "<unset>" : config_.elfPath.c_str(),
        config_.memory.c_str(),
        config_.launchMode.c_str(),
        config_.cpuClock.c_str(),
        static_cast<unsigned long long>(
            config_.syncInstructionQuantum),
        network_ == nullptr ? "detached" : "attached",
        static_cast<unsigned>(config_.networkSize));

    if (analogDevice_ != nullptr) {
        output_.verbose(
            CALL_INFO,
            1,
            0,
            "configured tile %u analog device (%zu arrays, size=%ux%u, backend=%s, link=%s, width=%u bits, compute_latency=%llu cycles)\n",
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

    if (config_.cpuClock.empty()) {
        output_.fatal(CALL_INFO, -1,
                      "tile %u has an empty cpu_clock\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (config_.syncInstructionQuantum == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "tile %u requires sync_instruction_quantum to be nonzero\n",
            static_cast<unsigned>(config_.tileId));
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
            event->instructionsExecuted >
                config_.syncInstructionQuantum) {
            output_.fatal(
                CALL_INFO,
                -1,
                "tile %u received invalid fd 41 event "
                "(epoch=%llu, expected=%llu, executed=%llu, previous=%llu, quantum=%llu)\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(event->grantEpoch),
                static_cast<unsigned long long>(currentGrantEpoch_),
                static_cast<unsigned long long>(
                    event->instructionsExecuted),
                static_cast<unsigned long long>(
                    lastGrantInstruction_),
                static_cast<unsigned long long>(
                    config_.syncInstructionQuantum));
        }

        const std::uint64_t instructionCycles =
            event->instructionsExecuted - lastGrantInstruction_;
        lastGrantInstruction_ = event->instructionsExecuted;
        synchronizedInstructions_ += instructionCycles;
        ++synchronizationEvents_;
        pendingSyncEvent_ = *event;

        output_.verbose(
            CALL_INFO,
            3,
            0,
            "tile %u fd 41 event %llu: %s after %llu instructions in grant %llu\n",
            static_cast<unsigned>(config_.tileId),
            static_cast<unsigned long long>(
                event->eventSequence),
            syncStopReasonName(event->stopReason),
            static_cast<unsigned long long>(
                event->instructionsExecuted),
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

    case MITTENS_SYNC_STOP_GUEST_EXIT: {
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
    state_ = LifecycleState::Exited;
    pendingSyncEvent_.reset();
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
        output_.fatal(
            CALL_INFO,
            -1,
            "QEMU tile %u failed with %s\n",
            static_cast<unsigned>(config_.tileId),
            status.describe().c_str());
    }

    primaryComponentOKToEndSim();
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
        serviceIncomingPackets();
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

                const std::uint64_t ticket = analogDevice_->submit(
                    submission->command,
                    std::move(submission->inputWords));
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
        analogRequests_.erase(request);
    }
}

void Tile::serviceOutgoingPackets()
{
    constexpr int kVirtualNetwork = 0;
    constexpr int kPacketBits = 32;

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
