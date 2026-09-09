#include "sst_config.h"

#include "tile.h"
#include "../profiling/tileProgress.h"

#include "../analog/crossSimAnalogBackend.h"
#include "../analog/nativeAnalogBackend.h"
#include "../analog/timingAnalogBackend.h"
#include "../memory/memoryAccessCoalescer.h"
#include "../network/packetEvent.h"
#include "../memory/globalRAMBacking.h"
#include "../execution/qemuReadySetExecutor.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>

#include <unistd.h>

namespace SST
{
namespace Mittens
{

namespace
{

// SST owns the request only after send(); constructor failures and rejected
// issues release it without a Tile-side pending collection.
class StandardMemoryRequest final : public MemoryAccessController::PreparedRequest
{
  public:
    StandardMemoryRequest(SST::Interfaces::StandardMem* port, std::uint64_t address,
                          std::uint32_t bytes, bool write)
        : port_(port)
    {
        if (write)
            request_ = std::make_unique<SST::Interfaces::StandardMem::Write>(
                address, bytes, std::vector<std::uint8_t>(bytes, 0));
        else
            request_ = std::make_unique<SST::Interfaces::StandardMem::Read>(address, bytes);
        id_ = request_->getID();
    }
    std::uint64_t id() const noexcept override
    {
        return id_;
    }
    void send() override
    {
        port_->send(request_.release());
    }

  private:
    SST::Interfaces::StandardMem* port_;
    std::unique_ptr<SST::Interfaces::StandardMem::Request> request_;
    std::uint64_t id_;
};

constexpr auto kSyncWaitTimeout = std::chrono::milliseconds(50);
constexpr std::uint32_t kDeploymentFrameMagic = UINT32_C(0x474f4c4d);
constexpr std::size_t kDeploymentFrameHeaderWords = 7;

// A runtime ready set must contain every ordinary component event at one
// modeled frontier.  A default-priority self event can run immediately after
// the first tile submits and fragment the frontier into mostly scalar batches.
// Put the dispatch one priority behind ordinary events, but ahead of SST's
// barriers and one-shots, so every causally ready tile submits before any host
// capture starts.  The dispatcher is local-only and never crosses a link.
class RuntimeQemuReadySetDispatchEvent final : public SST::Event
{
  public:
    RuntimeQemuReadySetDispatchEvent()
    {
        setPriority(EVENTPRIORITY + 1);
    }
};

class AnalogWakeEvent final : public SST::Event
{
  public:
    explicit AnalogWakeEvent(std::uint64_t generation) : generation_(generation)
    {
        // Match the priority of the clock callback this event replaces, so a
        // completion at one tick remains visible before ordinary events at
        // that same tick.
        setPriority(CLOCKPRIORITY);
    }

    std::uint64_t generation() const noexcept
    {
        return generation_;
    }

  private:
    std::uint64_t generation_;
};

class TransmitDMACompletionEvent final : public SST::Event
{
  public:
    explicit TransmitDMACompletionEvent(std::uint32_t stream) : stream_(stream) {}
    std::uint32_t stream() const noexcept
    {
        return stream_;
    }

  private:
    std::uint32_t stream_ = 0;
};

constexpr std::uint32_t kCpuWakeInitial = UINT32_MAX;
constexpr std::uint32_t kCpuWakeGlobalDMACompletion = UINT32_MAX - 1U;
constexpr std::uint32_t kCpuWakeEpochRelease = UINT32_MAX - 2U;
constexpr std::uint32_t kCpuWakeNoPendingStop = UINT32_MAX - 3U;
constexpr std::uint32_t kCpuWakeMemoryInitializationRelease = UINT32_MAX - 4U;

class ProgressWatchdogEvent final : public SST::Event
{
  public:
    explicit ProgressWatchdogEvent(std::uint64_t generation) : generation_(generation) {}

    std::uint64_t generation() const noexcept
    {
        return generation_;
    }

    std::string toString() const override
    {
        return "mittens.cpu-sync.watchdog";
    }

  private:
    std::uint64_t generation_;
};

class CpuScheduledWakeEvent final : public SST::Event
{
  public:
    CpuScheduledWakeEvent(std::uint32_t source, std::uint64_t generation)
        : source_(source), generation_(generation)
    {
    }
    std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    std::string toString() const override
    {
        return "mittens.cpu-sync." + std::string(syncStopReasonName(source_));
    }

  private:
    std::uint32_t source_;
    std::uint64_t generation_;
};

class CpuWakeDiagnosticEvent final : public SST::Event
{
  public:
    explicit CpuWakeDiagnosticEvent(std::uint32_t source) : source_(source) {}

    std::string toString() const override
    {
        switch (source_)
        {
        case kCpuWakeInitial:
            return "mittens.cpu-sync.initial";
        case kCpuWakeGlobalDMACompletion:
            return "mittens.cpu-sync.global-dma-completion";
        case kCpuWakeEpochRelease:
            return "mittens.cpu-sync.epoch-release";
        case kCpuWakeNoPendingStop:
            return "mittens.cpu-sync.no-pending-stop";
        case kCpuWakeMemoryInitializationRelease:
            return "mittens.cpu-sync.memory-initialization-release";
        default:
            return "mittens.cpu-sync.stop=" + std::to_string(source_);
        }
    }

  private:
    std::uint32_t source_;
};

SST::Event* cpuWakeEvent(std::uint32_t source)
{
    static const bool diagnostics = std::getenv("MITTENS_QEMU_WAKE_DIAGNOSTICS") != nullptr;
    return diagnostics ? static_cast<SST::Event*>(new CpuWakeDiagnosticEvent(source))
                       : new SST::Event();
}

std::atomic<std::uint64_t> deploymentProgressEpoch{0};
std::atomic<std::uint64_t> memoryInitializationExecutionProgressEpoch{0};

} // namespace

Tile::Tile(SST::ComponentId_t id, SST::Params& params)
    : SST::Component(id), config_(Configuration::read(params)),
      output_("mittens: ", config_.verbosity, 0, SST::Output::STDOUT), network_(nullptr),
      memoryInterface_(nullptr),
      taskTrace_(config_.tileId, config_.taskTraceDirectory,
                 [this](const std::string& text) { output_.output("%s", text.c_str()); }),
      state_(LifecycleState::Constructed)
{
    std::unique_ptr<ScratchpadTimingModel> scratchpadTimingModel;
    std::uint64_t qemuReadySetPartitionKey_ = 0;
    std::uint32_t qemuReadySetPartitionWorkers_ = 1;
    config_.validate(output_);
    if (const char* directory = std::getenv("GOLEM_RESOLVED_CONFIG_DIR"))
    {
        resolvedConfigurationPath_ = config_.writeResolved(directory);
    }
    if (resolvedConfigurationPath_.empty() && !config_.profileOutputDirectory.empty())
        resolvedConfigurationPath_ =
            config_.writeResolved(config_.profileOutputDirectory + "/resolved");
    scratchpadServiceStatistic_ = registerStatistic<std::uint64_t>("scratchpad_service_cycles");
    scratchpadReadServiceStatistic_ =
        registerStatistic<std::uint64_t>("scratchpad_read_service_cycles");
    scratchpadWriteServiceStatistic_ =
        registerStatistic<std::uint64_t>("scratchpad_write_service_cycles");
    const RankInfo readySetRank = getRank();
    const RankInfo readySetRanks = getNumRanks();
    qemuReadySetPartitionKey_ =
        (static_cast<std::uint64_t>(readySetRank.rank) << 32U) | readySetRank.thread;
    if (config_.qemuReadySetWorkers > 1)
    {
        if (readySetRanks.thread == 0 || config_.qemuReadySetWorkers < readySetRanks.thread)
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u requires the global QEMU ready-set worker budget "
                          "(%u) to cover every SST thread (%u)\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(config_.qemuReadySetWorkers),
                          static_cast<unsigned>(readySetRanks.thread));
        }
        qemuReadySetPartitionWorkers_ =
            config_.qemuReadySetWorkers / readySetRanks.thread +
            (readySetRank.thread < config_.qemuReadySetWorkers % readySetRanks.thread ? 1U : 0U);
        try
        {
            QemuCaptureCoordinator::registerInitialQemuReadySetTile(
                qemuReadySetPartitionKey_, config_.tileId, config_.qemuReadySetWorkers,
                qemuReadySetPartitionWorkers_, config_.memoryInitializationBarrierTiles,
                config_.qemuReadySetIndependenceProof);
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "tile %u could not join the initial QEMU ready set: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    if (config_.qemuRuntimeReadySet)
    {
        if (readySetRanks.thread != 1)
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u runtime QEMU ready-set execution is not yet "
                          "partition-local for %u SST threads\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(readySetRanks.thread));
        }
        try
        {
            QemuCaptureCoordinator::registerRuntimeQemuReadySetTile(
                config_.tileId, config_.qemuReadySetWorkers,
                config_.memoryInitializationBarrierTiles, config_.qemuReadySetIndependenceProof);
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "tile %u could not join the runtime QEMU ready set: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    if (config_.qemuLocalLookahead)
    {
        try
        {
            QemuCaptureCoordinator::registerLocalQemuLookaheadTile(
                config_.tileId, config_.qemuReadySetWorkers,
                config_.memoryInitializationBarrierTiles, config_.qemuReadySetIndependenceProof);
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "tile %u could not join local QEMU lookahead: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    if (config_.scratchpadEnabled)
    {
        try
        {
            scratchpadTimingModel =
                std::make_unique<ScratchpadTimingModel>(ScratchpadTimingConfiguration{
                    config_.scratchpadBytes,
                    config_.scratchpadBanks,
                    config_.scratchpadReadPorts,
                    config_.scratchpadWritePorts,
                    config_.scratchpadAccessWidthBits,
                    config_.scratchpadLatencyCycles,
                    config_.scratchpadDMASetupCycles,
                    config_.scratchpadDMABytesPerCycle,
                });
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "tile %u has an invalid scratchpad configuration: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    try
    {
        performanceProfile_.configure(config_.tileId,
                                      config_.profileMode == "off" ? std::string()
                                                                   : config_.profileOutputDirectory,
                                      config_.profileMode == "trace");
    }
    catch (const std::exception& error)
    {
        QemuProcess::terminateAll();
        output_.fatal(CALL_INFO, -1, "tile %u could not configure performance profiling: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    network_ = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
        "networkIF", SST::ComponentInfo::SHARE_NONE, 1);

    if (scratchpadTimingModel && performanceProfile_.traceEnabled())
        scratchpadTimingModel->setObserver([this](const ScratchpadBeatObservation& beat) {
            performanceProfile_.recordScratchpadBeat(beat);
        });

    if (network_ != nullptr && !managedLaunch())
    {
        output_.fatal(CALL_INFO, -1, "tile %u requires managed launch when networkIF is attached\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (network_ != nullptr && config_.networkSize == 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u requires a nonzero network_size when networkIF is attached\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (network_ != nullptr)
    {
        network_->setNotifyOnSend(
            new SST::Interfaces::SimpleNetwork::Handler<Tile, &Tile::handleNetworkSend>(this));
        network_->setNotifyOnReceive(
            new SST::Interfaces::SimpleNetwork::Handler<Tile, &Tile::handleNetworkReceive>(this));
    }

    if (config_.memoryBackend == "memhierarchy")
    {
        memoryClockTimeBase_ = getTimeConverter(config_.cpuClock);
        memoryInterface_ = loadUserSubComponent<SST::Interfaces::StandardMem>(
            "memoryIF", SST::ComponentInfo::SHARE_NONE, memoryClockTimeBase_,
            new SST::Interfaces::StandardMem::Handler<Tile, &Tile::handleMemoryResponse>(this));
        if (memoryInterface_ == nullptr)
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u requires a StandardMem memoryIF when "
                          "memory_backend=memhierarchy\n",
                          static_cast<unsigned>(config_.tileId));
        }
    }
    if (config_.networkSize != 0 && config_.tileId >= config_.networkSize)
    {
        output_.fatal(CALL_INFO, -1, "tile ID %u is outside network_size %u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(config_.networkSize));
    }

    if (managedLaunch())
    {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
        cpuClockTimeBase_ = getTimeConverter(config_.cpuClock);
        cpuSyncLink_ =
            configureSelfLink("qemu-sync", cpuClockTimeBase_,
                              new SST::Event::Handler<Tile, &Tile::handleCpuSyncEvent>(this));
        if (cpuSyncLink_ == nullptr)
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u failed to configure its QEMU synchronization link\n",
                          static_cast<unsigned>(config_.tileId));
        }
        if (config_.qemuRuntimeReadySet)
        {
            runtimeQemuReadySetLink_ = configureSelfLink(
                "qemu-runtime-ready-set", cpuClockTimeBase_,
                new SST::Event::Handler<Tile, &Tile::handleRuntimeQemuReadySetDispatch>(this));
            if (runtimeQemuReadySetLink_ == nullptr)
            {
                output_.fatal(CALL_INFO, -1,
                              "tile %u failed to configure its runtime QEMU "
                              "ready-set link\n",
                              static_cast<unsigned>(config_.tileId));
            }
        }
        globalDMALink_ = configureLink(
            "globalDMA", new SST::Event::Handler<Tile, &Tile::handleGlobalDMAEvent>(this));
        memoryInitializationBarrierLink_ = configureLink(
            "memoryInitBarrier",
            new SST::Event::Handler<Tile, &Tile::handleMemoryInitializationBarrierEvent>(this));
        if ((config_.memoryInitializationBarrierTiles == 0) !=
            (memoryInitializationBarrierLink_ == nullptr))
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u requires memory_init_barrier_tiles and the memoryInitBarrier "
                          "link to be configured together\n",
                          static_cast<unsigned>(config_.tileId));
        }
        epochBarrierLink_ = configureLink(
            "epochBarrier", new SST::Event::Handler<Tile, &Tile::handleEpochBarrierEvent>(this));
        if ((config_.epochBarrierEpochs == 0) != (epochBarrierLink_ == nullptr))
        {
            output_.fatal(CALL_INFO, -1,
                          "tile %u requires epoch_barrier_epochs and the epochBarrier link to be "
                          "configured together\n",
                          static_cast<unsigned>(config_.tileId));
        }
        if (network_ != nullptr)
        {
            networkClockTimeBase_ = getTimeConverter(config_.meshLinkClock);
            networkCompletionLink_ = configureSelfLink(
                "network-completion", networkClockTimeBase_,
                new SST::Event::Handler<Tile, &Tile::handleNetworkCompletionEvent>(this));
            if (networkCompletionLink_ == nullptr)
            {
                output_.fatal(CALL_INFO, -1,
                              "tile %u failed to configure its network "
                              "completion link\n",
                              static_cast<unsigned>(config_.tileId));
            }
            receiveDMALink_ = configureSelfLink(
                "rx-dma", cpuClockTimeBase_,
                new SST::Event::Handler<Tile, &Tile::handleReceiveDMAEvent>(this));
            if (receiveDMALink_ == nullptr)
            {
                output_.fatal(CALL_INFO, -1, "tile %u failed to configure its receive DMA link\n",
                              static_cast<unsigned>(config_.tileId));
            }
            if (config_.transmitDMAStreams == 1)
            {
                transmitDMALink_ = configureSelfLink(
                    "tx-dma", cpuClockTimeBase_,
                    new SST::Event::Handler<Tile, &Tile::handleTransmitDMAEvent>(this));
                if (transmitDMALink_ == nullptr)
                {
                    output_.fatal(CALL_INFO, -1,
                                  "tile %u failed to configure its transmit DMA link\n",
                                  static_cast<unsigned>(config_.tileId));
                }
            }
            else
            {
                transmitDMAStreamLinks_.resize(config_.transmitDMAStreams);
                for (std::uint32_t stream = 0; stream < config_.transmitDMAStreams; ++stream)
                {
                    transmitDMAStreamLinks_[stream] = configureSelfLink(
                        "tx-dma-" + std::to_string(stream), cpuClockTimeBase_,
                        new SST::Event::Handler<Tile, &Tile::handleTransmitDMAEvent>(this));
                    if (transmitDMAStreamLinks_[stream] == nullptr)
                    {
                        output_.fatal(
                            CALL_INFO, -1, "tile %u failed to configure TX DMA stream %u\n",
                            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(stream));
                    }
                }
            }
        }
    }

    TxController::Host txHost;
    txHost.now = [this] { return Timing::Ticks{getCurrentSimCycle()}; };
    txHost.meshHops = [this](std::uint32_t source, std::uint32_t destination)
    { return meshHops(source, destination); };
    if (transmitDMALink_ != nullptr)
    {
        txHost.scheduleDMACompletion =
            [link = transmitDMALink_](std::uint32_t, Timing::Cycles<Timing::Cpu> delay)
        { link->send(delay.value, new SST::Event()); };
    }
    else if (!transmitDMAStreamLinks_.empty())
    {
        txHost.scheduleDMACompletion = [links = transmitDMAStreamLinks_](
                                           std::uint32_t stream, Timing::Cycles<Timing::Cpu> delay)
        { links.at(stream)->send(delay.value, new TransmitDMACompletionEvent(stream)); };
    }
    tx_ = std::make_unique<TxController>(
        TxController::Configuration{
            config_.tileId,
            config_.networkSize,
            config_.meshWidth,
            config_.meshHeight,
            config_.networkPacketWords,
            config_.transmitDMAStreams,
            config_.transmitDMAFIFOBytes,
            config_.scratchpadDMABytesPerCycle,
            scratchpadRegion(),
            kDeploymentFrameMagic,
            kDeploymentFrameHeaderWords,
        },
        TxController::Resources{
            bridge_,
            network_,
            scratchpadTimingModel.get(),
            performanceProfile_,
            output_,
            Timing::Clock<Timing::Cpu>(getTimeConverter(config_.cpuClock).getFactor()),
        },
        std::move(txHost));

    RxController::Host rxHost;
    rxHost.now = [this] { return Timing::Ticks{getCurrentSimCycle()}; };
    rxHost.meshHops = [this](std::uint32_t source, std::uint32_t destination)
    { return meshHops(source, destination); };
    if (receiveDMALink_ != nullptr)
    {
        rxHost.scheduleDMACompletion = [link = receiveDMALink_](Timing::Cycles<Timing::Cpu> delay)
        { link->send(delay.value, new SST::Event()); };
    }
    if (networkCompletionLink_ != nullptr)
    {
        rxHost.scheduleNetworkCompletion =
            [link = networkCompletionLink_](Timing::Cycles<Timing::Network> delay)
        { link->send(delay.value, new SST::Event()); };
    }
    rx_ = std::make_unique<RxController>(
        RxController::Configuration{
            config_.tileId,
            config_.networkSize,
            config_.receiveDMAQueueDepth,
            config_.receiveDMAWidthBits,
            config_.receiveDMASetupCycles,
            config_.scratchpadEnabled,
            config_.scratchpadBytes,
            config_.memoryGuestBase,
            config_.memoryTileStride,
            config_.memoryCacheLineSize,
            config_.networkTailDelivery,
            config_.meshLinkWidthBits,
            kDeploymentFrameMagic,
            kDeploymentFrameHeaderWords,
            config_.receiveDMAStreaming,
            config_.receiveDMAStreams,
        },
        RxController::Resources{
            bridge_,
            network_,
            memoryInterface_,
            scratchpadTimingModel.get(),
            performanceProfile_,
            output_,
            Timing::Clock<Timing::Cpu>(getTimeConverter(config_.cpuClock).getFactor()),
            Timing::Clock<Timing::ReceiveDMA>(
                getTimeConverter(config_.receiveDMAClock).getFactor()),
            Timing::Clock<Timing::Network>(getTimeConverter(config_.meshLinkClock).getFactor()),
        },
        std::move(rxHost));

    if (config_.analogArrayCount != 0)
    {
        analogClockTimeBase_ = getTimeConverter(config_.analogLinkClock);
        analogWakeLink_ =
            configureSelfLink("analog-wake", analogClockTimeBase_,
                              new SST::Event::Handler<Tile, &Tile::handleAnalogWakeEvent>(this));
        if (analogWakeLink_ == nullptr)
        {
            output_.fatal(CALL_INFO, -1, "tile %u failed to configure its analog wake link\n",
                          static_cast<unsigned>(config_.tileId));
        }
        const SST::SimTime_t factor = analogClockTimeBase_.getFactor();
        if (factor == 0)
        {
            output_.fatal(CALL_INFO, -1, "tile %u has a zero analog clock factor\n",
                          static_cast<unsigned>(config_.tileId));
        }
    }
    DeviceDiagnostics diagnostics{[this](int level, const std::string& text)
                                  { output_.verbose(CALL_INFO, level, 0, "%s", text.c_str()); }};
    MemoryAccessController::Host memoryHost;
    memoryHost.now = [this] { return Timing::Ticks{getCurrentSimCycle()}; };
    memoryHost.context = [this]
    {
        const auto context = taskTrace_.context();
        return MemoryAccessController::Context{context.task.value_or(UINT32_MAX), context.execution,
                                               context.task ? "task" : "runtime"};
    };
    if (memoryInterface_)
        memoryHost.prepare =
            [port = memoryInterface_](std::uint64_t address, std::uint32_t bytes, bool write)
        { return std::make_unique<StandardMemoryRequest>(port, address, bytes, write); };
    memoryHost.pending = [this] { return cpu_->pending(); };
    memoryHost.hasMemoryReplay = [this] { return cpu_->hasMemoryReplay(); };
    memoryHost.completeMemory = [this](std::uint64_t step, bool group)
    { cpu_->completeMemory(step, group); };
    memoryHost.retry = [this] { return cpu_->processPendingSyncEvent(); };
    memoryAccess_ = std::make_unique<MemoryAccessController>(
        config_, Timing::Clock<Timing::Cpu>(getTimeConverter(config_.cpuClock).getFactor()),
        std::move(scratchpadTimingModel), performanceProfile_, diagnostics, std::move(memoryHost));

    AnalogController::Host analogHost;
    analogHost.now = [this] { return Timing::Ticks{getCurrentSimCycle()}; };
    analogHost.active = [this]
    { return state_ != LifecycleState::Exited && state_ != LifecycleState::Finished; };
    analogHost.pending = [this] { return cpu_->pending(); };
    analogHost.pendingStepId = [this] { return cpu_->pendingStepId(); };
    analogHost.hasAnalogReplay = [this] { return cpu_->hasAnalogReplay(); };
    analogHost.completeDevice = [this](std::uint64_t step) { cpu_->completeDevice(step); };
    if (analogWakeLink_)
        analogHost.scheduleWake =
            [link = analogWakeLink_](Timing::Cycles<Timing::Analog> delay, std::uint64_t generation)
        { link->send(delay.value, new AnalogWakeEvent(generation)); };
    analog_ = std::make_unique<AnalogController>(
        config_,
        Timing::Clock<Timing::Analog>(getTimeConverter(config_.analogLinkClock).getFactor()),
        performanceProfile_, diagnostics, std::move(analogHost));

    GlobalDMAClient::Host dmaHost;
    dmaHost.now = [this] { return Timing::Ticks{getCurrentSimCycle()}; };
    dmaHost.scratchpadAvailable = [this] { return memoryAccess_->scratchpadAvailable(); };
    dmaHost.reserveDMA =
        [this](std::uint64_t cursor, std::uint64_t offset, std::uint64_t bytes, bool write)
    { return memoryAccess_->reserveDMA(cursor, offset, bytes, write); };
    if (globalDMALink_)
        dmaHost.send = [link = globalDMALink_](GlobalDMAMessage m)
        {
            link->send(new GlobalDMAEvent(m.tileId(), m.executionId(), m.tokenId(),
                                          m.logicalIteration(), m.globalOffset(),
                                          m.scratchpadOffset(), m.byteCount(), m.direction(),
                                          m.requestFlags(), m.completion()));
        };
    dmaHost.pending = [this] { return cpu_->pending(); };
    dmaHost.wake = [this] { cpuSyncLink_->send(cpuWakeEvent(kCpuWakeGlobalDMACompletion)); };
    globalDMA_ = std::make_unique<GlobalDMAClient>(
        config_, Timing::Clock<Timing::Cpu>(getTimeConverter(config_.cpuClock).getFactor()),
        diagnostics, std::move(dmaHost));

    InitializationBarrierClient::Host barrierHost;
    barrierHost.running = [this] { return state_ == LifecycleState::Running; };
    barrierHost.globalDMADrained = [this] { return globalDMA_->drained(); };
    barrierHost.pending = [this] { return cpu_->pending(); };
    barrierHost.completeWait = [this] { cpu_->completeWait(); };
    if (memoryInitializationBarrierLink_)
        barrierHost.sendInitialization =
            [link = memoryInitializationBarrierLink_, tile = config_.tileId]
        {
            link->send(new MemoryInitializationBarrierEvent(
                tile, MemoryInitializationBarrierMessage::Arrive));
        };
    if (epochBarrierLink_)
        barrierHost.sendEpoch = [link = epochBarrierLink_, tile = config_.tileId](
                                    std::uint32_t epoch, EpochBarrierContribution contribution)
        {
            link->send(
                new EpochBarrierEvent(tile, epoch, EpochBarrierMessage::Arrive, contribution));
        };
    barrierHost.wakeInitialization = [this]
    { cpuSyncLink_->send(cpuWakeEvent(kCpuWakeMemoryInitializationRelease)); };
    barrierHost.wakeEpoch = [this] { cpuSyncLink_->send(cpuWakeEvent(kCpuWakeEpochRelease)); };
    barriers_ =
        std::make_unique<InitializationBarrierClient>(config_, diagnostics, std::move(barrierHost));

    output_.verbose(
        CALL_INFO, 1, 0,
        "configured tile %u (qemu=%s, elf=%s, qemu_control_memory=%s, "
        "memory_backend=%s/init-batch-%s/memory-access-batch-%s/scratchpad-access-batch-%s/"
        "run-compact-%s/event-batch-%s/global-dma-submit-batch-%s/global-dma-macro-%s/"
        "analog-command-batch-%s-%u/%uB-cycle/setup-%llu, launch=%s, cpu_clock=%s, issue_width=%u, "
        "sync_quantum=%llu, qemu_ready_set=%u/runtime-%s/lookahead-%s/spin-%uus, "
        "rvv=%s/vlen-%u/elen-%u, network=%s, network_size=%u, mesh_link=%s/%u-bit/packet-%u-words, "
        "rx_dma=%s/%u-bit/setup-%llu/queue-%u)\n",
        static_cast<unsigned>(config_.tileId), config_.qemuPath.c_str(),
        config_.elfPath.empty() ? "<unset>" : config_.elfPath.c_str(), config_.memory.c_str(),
        config_.memoryBackend.c_str(), config_.memoryInitializationBatching ? "on" : "off",
        config_.memoryAccessBatching ? "on" : "off",
        config_.scratchpadAccessBatching ? "on" : "off",
        config_.scratchpadAccessRunCompaction ? "on" : "off",
        config_.memoryEventBatching ? "on" : "off", config_.globalDMASubmitBatching ? "on" : "off",
        config_.globalDMAMacroExecution ? "on" : "off",
        config_.analogCommandBatching ? "on" : "off",
        static_cast<unsigned>(config_.memoryAccessBatchRecords),
        static_cast<unsigned>(config_.memoryInitializationBytesPerCycle),
        static_cast<unsigned long long>(config_.memoryInitializationLatencyCycles),
        config_.launchMode.c_str(), config_.cpuClock.c_str(),
        static_cast<unsigned>(config_.cpuIssueWidth),
        static_cast<unsigned long long>(config_.syncInstructionQuantum),
        static_cast<unsigned>(config_.qemuReadySetWorkers),
        config_.qemuRuntimeReadySet ? "on" : "off", config_.qemuLocalLookahead ? "on" : "off",
        static_cast<unsigned>(config_.qemuCaptureSpinMicroseconds),
        config_.riscvVectorEnabled ? "enabled" : "disabled",
        static_cast<unsigned>(config_.riscvVectorLengthBits),
        static_cast<unsigned>(config_.riscvVectorElementBits),
        network_ == nullptr ? "detached" : "attached", static_cast<unsigned>(config_.networkSize),
        config_.meshLinkClock.c_str(), static_cast<unsigned>(config_.meshLinkWidthBits),
        static_cast<unsigned>(config_.networkPacketWords), config_.receiveDMAClock.c_str(),
        static_cast<unsigned>(config_.receiveDMAWidthBits),
        static_cast<unsigned long long>(config_.receiveDMASetupCycles),
        static_cast<unsigned>(config_.receiveDMAQueueDepth));

    output_.verbose(CALL_INFO, 1, 0,
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
                    static_cast<unsigned long long>(config_.scratchpadLatencyCycles),
                    static_cast<unsigned>(config_.scratchpadDMABytesPerCycle),
                    static_cast<unsigned long long>(config_.scratchpadDMASetupCycles));

    if (analog_->enabled())
    {
        output_.verbose(CALL_INFO, 1, 0,
                        "configured tile %u analog device (%zu arrays, size=%ux%u, backend=%s, "
                        "shared_link=%s, width=%u bits, compute_latency=%llu cycles)\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<std::size_t>(config_.analogArrayCount),
                        static_cast<unsigned>(config_.analogArrayRows),
                        static_cast<unsigned>(config_.analogArrayColumns),
                        config_.analogBackend.c_str(), config_.analogLinkClock.c_str(),
                        static_cast<unsigned>(MITTENS_ANALOG_LINK_WIDTH_BITS),
                        static_cast<unsigned long long>(config_.analogComputeLatencyCycles));
    }
    CpuExecutionController::Transport transport;
    transport.grant = [bridge = &syncBridge_](std::uint64_t budget)
    { return bridge->grant(budget); };
    transport.resume = [bridge = &syncBridge_](const QemuSyncEvent& event)
    { bridge->resume(event); };
    transport.poll = [this]()
    {
        auto event = syncBridge_.waitForEvent(
            kSyncWaitTimeout, std::chrono::microseconds(config_.qemuCaptureSpinMicroseconds));
        checkSyncBridgeError();
        return event;
    };
    transport.captureHost =
        [bridge = &syncBridge_, process = &qemu_, watchdog = config_.progressWatchdogMilliseconds,
         spin = config_.qemuCaptureSpinMicroseconds](const std::atomic<bool>& active)
    {
        return QemuCaptureCoordinator::captureHost(*bridge, process->pid(), watchdog, spin, active);
    };
    CpuExecutionController::Host host;
    host.running = [this]() { return state_ == LifecycleState::Running; };
    host.initializing = [this]() { return barriers_->snapshot().memoryInitializationPhase_; };
    host.observeExit = [this]() { return observeQemuExit(); };
    host.watchdogReported = [this]() { return progressWatchdogReported_; };
    host.now = [this]() { return Timing::Ticks{getCurrentSimCycle()}; };
    host.watchdog = [this]() { maybeProgressWatchdog(); };
    host.terminateAll = []() { QemuProcess::terminateAll(); };
    host.scheduleCaptureDispatch = [this]()
    { runtimeQemuReadySetLink_->send(new RuntimeQemuReadySetDispatchEvent()); };
    host.serviceBridge = [this]() { serviceBridge(); };
    host.schedule =
        [this](Timing::Cycles<Timing::Cpu> cycles, std::uint32_t source, std::uint64_t generation)
    {
        if (cpuSyncLink_)
            cpuSyncLink_->send(cycles.value, new CpuScheduledWakeEvent(source, generation));
    };
    host.scheduleWatchdog = [this](Timing::Cycles<Timing::Cpu> cycles, std::uint64_t generation)
    {
        if (cpuSyncLink_)
            cpuSyncLink_->send(cycles.value, new ProgressWatchdogEvent(generation));
    };
    host.progress = [this](bool initializing)
    {
        if (initializing)
            observedMemoryInitializationExecutionProgressEpoch_ =
                memoryInitializationExecutionProgressEpoch.fetch_add(1, std::memory_order_relaxed) +
                1;
        else
            observedDeploymentProgressEpoch_ =
                deploymentProgressEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
        lastRetirementWallTime_ = std::chrono::steady_clock::now();
    };
    host.recordWait = [this](std::uint64_t start, std::uint64_t finish, const char* reason,
                             std::uint64_t sequence)
    { performanceProfile_.recordWait(start, finish, reason, sequence); };
    host.log = [this](int level, const std::string& text)
    { output_.verbose(CALL_INFO, level, 0, "%s", text.c_str()); };
    host.scratchpadAvailable = [this]() { return memoryAccess_->scratchpadAvailable(); };
    host.storesDrained = [this]() { return memoryAccess_->storesDrained(); };
    host.receiveReady = [this]() { return rx_->readyForGuest(); };
    host.transmitReady = [this](bool burst) { return tx_->readyForGuest(burst); };
    host.scratchpad = [this](const MittensSyncMemoryAccess& access, std::uint64_t cursor)
    { return memoryAccess_->reserveCPU(access, cursor); };
    host.analogArrayCount = [this] { return analog_->arrayCount(); };
    host.analogSubmitted = [this](std::uint32_t array, std::uint64_t sequence)
    { return analog_->submitted(array, sequence); };
    host.prepareDeferredAnalog = [this](const QemuSyncEvent& e)
    { return analog_->startDeferredAnalogLoadLookahead(e); };
    host.deferredAnalogMatches = [this](const QemuSyncEvent& e)
    { return analog_->deferredMatches(e); };
    host.validateInitialDevice = [this](const QemuSyncEvent& e)
    { analog_->validateInitialCpuDevice(e); };
    host.memory = [this](const CpuMemoryAction& a) { return memoryAccess_->executeCpuMemory(a); };
    host.analog = [this](const CpuAnalogAction& a) { return analog_->executeCpuAnalog(a); };
    host.network = [this](const CpuNetworkAction& a) { return executeCpuNetwork(a); };
    host.globalDMA = [this](const CpuGlobalDMAAction& a)
    { return globalDMA_->executeCpuGlobalDMA(a); };
    host.barrier = [this](const CpuBarrierAction& a) { return barriers_->executeCpuBarrier(a); };
    host.initialization = [this](const CpuInitializationAction& a)
    { return barriers_->executeCpuInitialization(a); };
    host.task = [this](const CpuTaskAction& a) { return executeCpuTask(a); };
    host.guestExit = [this]() { executeCpuExit(); };
    // Disabled-launch lifecycle fixtures have no CPU link, but the execution
    // owner still needs the configured domain for snapshots and shutdown.
    if (!managedLaunch())
        cpuClockTimeBase_ = getTimeConverter(config_.cpuClock);
    cpu_ = std::make_unique<CpuExecutionController>(config_, cpuDomain(), std::move(transport),
                                                    std::move(host), qemuReadySetPartitionKey_,
                                                    qemuReadySetPartitionWorkers_);
}

Tile::~Tile()
{
    // Workers borrow mappings/process endpoints: revoke and join first.
    if (cpu_)
        cpu_->stop();
    qemu_.terminate();
}

bool Tile::managedLaunch() const
{
    return config_.launchMode == "managed";
}

void Tile::init(unsigned phase)
{
    if (state_ == LifecycleState::Finished)
    {
        output_.fatal(CALL_INFO, -1, "tile %u was initialized after finish()\n",
                      static_cast<unsigned>(config_.tileId));
    }

    state_ = LifecycleState::Initializing;

    if (network_ != nullptr)
    {
        network_->init(phase);
    }
    if (memoryInterface_ != nullptr)
    {
        memoryInterface_->init(phase);
    }

    output_.verbose(CALL_INFO, 3, 0, "tile %u init phase %u\n",
                    static_cast<unsigned>(config_.tileId), phase);
}

void Tile::complete(unsigned phase)
{
    if (memoryInterface_ != nullptr)
    {
        memoryInterface_->complete(phase);
    }
}

void Tile::setup()
{
    if (state_ == LifecycleState::Finished)
    {
        output_.fatal(CALL_INFO, -1, "tile %u entered setup() after finish()\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (managedLaunch())
    {
        try
        {
            syncBridge_.create(config_.tileId);
            globalRAMFileDescriptor_.reset(GlobalRAMBacking::duplicate(config_.globalRAMBytes));
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1,
                          "failed to create synchronization bridge for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }

    if (network_ != nullptr)
    {
        network_->setup();

        try
        {
            bridge_.create(config_.tileId);
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "failed to create bridge for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
    }
    if (memoryInterface_ != nullptr)
    {
        memoryInterface_->setup();
    }
    deviceNotification([&] { analog_->setup(); });

    state_ = LifecycleState::Setup;
    output_.verbose(CALL_INFO, 2, 0, "tile %u setup complete\n",
                    static_cast<unsigned>(config_.tileId));

    if (managedLaunch())
    {
        output_.verbose(CALL_INFO, 1, 0, "starting QEMU for tile %u\n",
                        static_cast<unsigned>(config_.tileId));

        try
        {
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
                config_.memoryAccessBatching,
                config_.scratchpadAccessBatching,
                config_.scratchpadAccessRunCompaction,
                config_.memoryEventBatching,
                config_.globalDMASubmitBatching,
                config_.globalDMAMacroExecution,
                config_.analogCommandBatching,
                config_.memoryAccessBatchRecords,
                config_.scratchpadEnabled,
                MITTENS_SCRATCHPAD_BASE,
                config_.scratchpadBytes,
                syncBridge_.fileDescriptor(),
                bridge_.fileDescriptor(),
                analog_->fileDescriptor(),
                globalRAMFileDescriptor_.get(),
                config_.globalRAMBytes,
                config_.serialOutputDirectory.empty() ? std::string()
                                                      : config_.serialOutputDirectory + "/tile-" +
                                                            std::to_string(config_.tileId) + ".log",
            });
            globalRAMFileDescriptor_.reset();
        }
        catch (const std::exception& error)
        {
            output_.fatal(CALL_INFO, -1, "failed to start QEMU for tile %u: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }

        state_ = LifecycleState::Running;
        barriers_->start();
        const auto now = std::chrono::steady_clock::now();
        lastProgressWallTime_ = now;
        lastRetirementWallTime_ = now;
        lastProgressSnapshotWallTime_ = now;
        observedDeploymentProgressEpoch_ = deploymentProgressEpoch.load(std::memory_order_relaxed);
        observedMemoryInitializationExecutionProgressEpoch_ =
            memoryInitializationExecutionProgressEpoch.load(std::memory_order_relaxed);
        progressWatchdogInitialized_ = true;
        output_.verbose(CALL_INFO, 2, 0, "QEMU tile %u started with PID %d\n",
                        static_cast<unsigned>(config_.tileId), static_cast<int>(qemu_.pid()));
        cpuSyncLink_->send(cpuWakeEvent(kCpuWakeInitial));
    }
}

void Tile::handleCpuSyncEvent(SST::Event* event)
{
    std::optional<std::uint64_t> generation;
    std::optional<std::uint64_t> scheduled;
    if (auto* watchdog = dynamic_cast<ProgressWatchdogEvent*>(event))
        generation = watchdog->generation();
    if (auto* wake = dynamic_cast<CpuScheduledWakeEvent*>(event))
        scheduled = wake->generation();
    delete event;
    try
    {
        cpu_->onWake(generation, scheduled);
    }
    catch (const std::exception& error)
    {
        QemuProcess::terminateAll();
        output_.fatal(CALL_INFO, -1, "tile %u CPU execution failed: %s\n", config_.tileId,
                      error.what());
    }
}

void Tile::handleRuntimeQemuReadySetDispatch(SST::Event* event)
{
    delete event;
    try
    {
        cpu_->dispatchCaptures();
    }
    catch (const std::exception& error)
    {
        QemuProcess::terminateAll();
        output_.fatal(CALL_INFO, -1, "tile %u runtime QEMU ready-set execution failed: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }
}

void Tile::handleGlobalDMAEvent(SST::Event* rawEvent)
{
    std::unique_ptr<SST::Event> owned(rawEvent);
    deviceNotification(
        [&]
        {
            auto* e = dynamic_cast<GlobalDMAEvent*>(rawEvent);
            if (!e)
                throw std::runtime_error("invalid global DMA event type");
            globalDMA_->onCompletion(
                GlobalDMAMessage(e->tileId(), e->executionId(), e->tokenId(), e->logicalIteration(),
                                 e->globalOffset(), e->scratchpadOffset(), e->byteCount(),
                                 e->direction(), e->requestFlags(), e->completion()));
        });
}
void Tile::handleMemoryInitializationBarrierEvent(SST::Event* rawEvent)
{
    std::unique_ptr<SST::Event> owned(rawEvent);
    deviceNotification(
        [&]
        {
            auto* e = dynamic_cast<MemoryInitializationBarrierEvent*>(rawEvent);
            if (!e)
                throw std::runtime_error("invalid initialization barrier event type");
            barriers_->onInitializationRelease(e->tileId(), e->message());
        });
}
void Tile::handleEpochBarrierEvent(SST::Event* rawEvent)
{
    std::unique_ptr<SST::Event> owned(rawEvent);
    deviceNotification(
        [&]
        {
            auto* e = dynamic_cast<EpochBarrierEvent*>(rawEvent);
            if (!e)
                throw std::runtime_error("invalid epoch barrier event type");
            if (!barriers_->onEpochRelease(e->tileId(), e->completedEpoch(), e->message(),
                                           e->contribution()))
                return;
            cpu_->stop();
            state_ = LifecycleState::Exited;
            // Prefix release has committed on this tile. Reap the deployment as
            // one process set, preserving the original nonserial teardown.
            QemuProcess::terminateAll();
            output_.output("MITTENS_EPOCH_PREFIX_TILE_STOP tile=%u releases=%u epoch_count=%u\n",
                           config_.tileId, barriers_->snapshot().expectedEpochBarrier_,
                           config_.epochBarrierEpochs);
            signalExitedTileIfDrained();
        });
}
void Tile::handleReceiveDMAEvent(SST::Event* event)
{
    delete event;
    if (rx_->onDMACompletion())
    {
        resumeReceiveWaitIfReady();
        resumeTransmitWaitIfReady();
    }
}

void Tile::handleTransmitDMAEvent(SST::Event* event)
{
    std::optional<std::uint32_t> stream;
    if (auto* completion = dynamic_cast<TransmitDMACompletionEvent*>(event))
    {
        stream = completion->stream();
    }
    delete event;
    tx_->onDMACompletion(stream);
    serviceOutgoingPackets();
    checkBridgeError();
    resumeTransmitWaitIfReady();
    signalExitedTileIfDrained();
}

void Tile::handleMemoryResponse(SST::Interfaces::StandardMem::Request* request)
{
    const auto invalidation = rx_->handleInvalidationResponse(request);
    if (invalidation != RxController::InvalidationResult::Unhandled)
    {
        if (invalidation == RxController::InvalidationResult::Authorized)
        {
            resumeReceiveWaitIfReady();
            resumeTransmitWaitIfReady();
        }
        return;
    }
    std::unique_ptr<SST::Interfaces::StandardMem::Request> owned(request);
    deviceNotification(
        [&]
        {
            if (!request)
                throw std::runtime_error("null memory response");
            const auto id = request->getID();
            owned.reset();
            memoryAccess_->onResponse(id);
        });
}

bool Tile::observeQemuExit()
{
    if (state_ != LifecycleState::Running)
    {
        return state_ == LifecycleState::Exited;
    }

    std::optional<QemuExitStatus> status;
    try
    {
        status = qemu_.pollExit();
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "failed to monitor QEMU tile %u: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }
    if (!status.has_value())
    {
        return false;
    }

    handleQemuExit(*status);
    return true;
}

void Tile::handleQemuExit(const QemuExitStatus& status)
{
    checkSyncBridgeError();
    cpu_->completeWait();
    state_ = LifecycleState::Exited;
    cpu_->stop();
    serviceBridge();

    output_.verbose(CALL_INFO, 1, 0,
                    "QEMU tile %u exited with %s; fd 41 synchronized "
                    "%llu instructions in %llu grants and %llu events\n",
                    static_cast<unsigned>(config_.tileId), status.describe().c_str(),
                    static_cast<unsigned long long>(cpu_->accounting().total.instructions),
                    static_cast<unsigned long long>(cpu_->statistics().synchronizationGrants_),
                    static_cast<unsigned long long>(cpu_->statistics().synchronizationEvents_));

    if (!status.success())
    {
        QemuProcess::terminateAll();
        output_.fatal(CALL_INFO, -1, "QEMU tile %u failed with %s\n",
                      static_cast<unsigned>(config_.tileId), status.describe().c_str());
    }
    barriers_->validateExit();

    signalExitedTileIfDrained();
}

void Tile::handleNetworkCompletionEvent(SST::Event* event)
{
    delete event;

    if (!bridge_.open() || network_ == nullptr)
    {
        return;
    }
    rx_->onNetworkCompletion();
    checkBridgeError();
    resumeReceiveWaitIfReady();
    resumeTransmitWaitIfReady();
    signalExitedTileIfDrained();
}

void Tile::handleAnalogWakeEvent(SST::Event* rawEvent)
{
    std::unique_ptr<SST::Event> owned(rawEvent);
    deviceNotification(
        [&]
        {
            auto* e = dynamic_cast<AnalogWakeEvent*>(rawEvent);
            if (!e)
                throw std::runtime_error("invalid analog wake event type");
            analog_->onWake(e->generation());
        });
}
bool Tile::handleNetworkSend(int)
{
    if (bridge_.open() && network_ != nullptr)
    {
        serviceOutgoingPackets();
        checkBridgeError();
        resumeTransmitWaitIfReady();
    }
    signalExitedTileIfDrained();
    return true;
}

bool Tile::handleNetworkReceive(int)
{
    if (bridge_.open() && network_ != nullptr)
    {
        rx_->service();
        checkBridgeError();
        resumeReceiveWaitIfReady();
        resumeTransmitWaitIfReady();
    }
    return true;
}

void Tile::serviceBridge()
{
    if (bridge_.open() && network_ != nullptr)
    {
        checkBridgeError();
        rx_->service();
        serviceOutgoingPackets();
        checkBridgeError();
    }

    analog_->serviceAnalogBridge();
}

void Tile::serviceOutgoingPackets()
{
    if (tx_->service())
    {
        signalExitedTileIfDrained();
    }
}

void Tile::checkBridgeError() const
{
    const std::uint32_t error = bridge_.protocolError();
    if (error != MITTENS_BRIDGE_ERROR_NONE)
    {
        output_.fatal(CALL_INFO, -1, "tile %u QEMU NIC reported bridge protocol error %u\n",
                      static_cast<unsigned>(config_.tileId), static_cast<unsigned>(error));
    }
}

void Tile::checkSyncBridgeError() const
{
    const std::uint32_t error = syncBridge_.protocolError();
    if (error != MITTENS_SYNC_BRIDGE_ERROR_NONE)
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u QEMU synchronization device reported bridge protocol error %u\n",
                      static_cast<unsigned>(config_.tileId), static_cast<unsigned>(error));
    }
}

bool Tile::outgoingPacketsIdle() const noexcept
{
    return tx_->idle();
}

void Tile::resumeTransmitWaitIfReady()
{
    cpu_->resumeTransmitWaitIfReady();
}

void Tile::resumeReceiveWaitIfReady()
{
    cpu_->resumeReceiveWaitIfReady();
}

std::uint32_t Tile::meshHops(std::uint32_t source, std::uint32_t destination) const noexcept
{
    if (source == destination)
    {
        return 0;
    }
    if (config_.meshWidth == 0 || config_.meshHeight == 0)
    {
        return 1;
    }
    const std::uint32_t sourceX = source % config_.meshWidth;
    const std::uint32_t sourceY = source / config_.meshWidth;
    const std::uint32_t destinationX = destination % config_.meshWidth;
    const std::uint32_t destinationY = destination / config_.meshWidth;
    const std::uint32_t horizontal =
        sourceX > destinationX ? sourceX - destinationX : destinationX - sourceX;
    const std::uint32_t vertical =
        sourceY > destinationY ? sourceY - destinationY : destinationY - sourceY;
    return horizontal + vertical;
}

void Tile::signalExitedTileIfDrained()
{
    if (state_ == LifecycleState::Exited && !primaryEndSignaled_ && outgoingPacketsIdle())
    {
        primaryEndSignaled_ = true;
        primaryComponentOKToEndSim();
    }
}

void Tile::recordProgressSnapshot(const char* kind)
{
    const auto now = std::chrono::steady_clock::now();
    const auto wallMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto progress = makeTileProgressSnapshot(
        measurementSnapshot(), rx_->status(), tx_->status(), memoryAccess_->pendingCount(),
        static_cast<std::uint64_t>(wallMilliseconds), kind);
    performanceProfile_.recordProgressSnapshot(progress);
    lastProgressWallTime_ = now;
    if (config_.verbosity >= 3 || (kind != nullptr && std::string(kind) == "watchdog"))
        output_.output("%s", formatTileProgress(config_.tileId, progress).c_str());
}

void Tile::maybeProgressWatchdog()
{
    if (!progressWatchdogInitialized_ || config_.progressWatchdogMilliseconds == 0 ||
        !qemu_.running() || progressWatchdogReported_)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t deploymentEpoch = deploymentProgressEpoch.load(std::memory_order_relaxed);
    if (deploymentEpoch != observedDeploymentProgressEpoch_)
    {
        observedDeploymentProgressEpoch_ = deploymentEpoch;
        lastRetirementWallTime_ = now;
    }
    const bool memoryInitializationBarrierPending =
        config_.memoryInitializationBarrierTiles != 0 &&
        barriers_->snapshot().memoryInitializationBarrierArrived_ &&
        !barriers_->snapshot().memoryInitializationBarrierReleaseReady_;
    const std::uint64_t initializationExecutionEpoch =
        memoryInitializationExecutionProgressEpoch.load(std::memory_order_relaxed);
    if (initializationExecutionEpoch != observedMemoryInitializationExecutionProgressEpoch_)
    {
        observedMemoryInitializationExecutionProgressEpoch_ = initializationExecutionEpoch;
        lastRetirementWallTime_ = now;
    }
    const auto snapshotElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressSnapshotWallTime_)
            .count();
    if (config_.progressSnapshotIntervalMilliseconds != 0 &&
        snapshotElapsed >= static_cast<std::int64_t>(config_.progressSnapshotIntervalMilliseconds))
    {
        recordProgressSnapshot("periodic");
        lastProgressSnapshotWallTime_ = now;
    }
    const auto retirementElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRetirementWallTime_)
            .count();
    const std::uint64_t watchdogMilliseconds =
        memoryInitializationBarrierPending && config_.progressWatchdogMilliseconds <=
                                                  std::numeric_limits<std::uint64_t>::max() / 10
            ? config_.progressWatchdogMilliseconds * 10
            : config_.progressWatchdogMilliseconds;
    if (retirementElapsed < static_cast<std::int64_t>(watchdogMilliseconds))
    {
        return;
    }
    progressWatchdogReported_ = true;
    recordProgressSnapshot("watchdog");
    output_.output("%s", formatProgressWatchdog(
        config_.tileId, watchdogMilliseconds, static_cast<std::uint64_t>(retirementElapsed),
        deploymentEpoch, initializationExecutionEpoch).c_str());
    qemu_.terminate();
    QemuProcess::terminateAll();
    output_.fatal(CALL_INFO, -1,
                  "tile %u progress watchdog fired after %llu ms without "
                  "deployment-wide architectural progress; diagnostics were flushed\n",
                  static_cast<unsigned>(config_.tileId),
                  static_cast<unsigned long long>(retirementElapsed));
}

TileMeasurementSnapshot Tile::measurementSnapshot() const
{
    TileMeasurementSnapshot data;
    data.configuration = config_;
    data.accounting = cpu_->accounting();
    data.cpu = cpu_->statistics();
    data.memory = memoryAccess_->statistics();
    data.globalDMA = globalDMA_->statistics();
    data.analog = analog_->statistics();
    data.barriers = barriers_->snapshot();
    data.tx = tx_->counters();
    data.rx = rx_->counters();
    data.scratchpad = memoryAccess_->scratchpadStatistics();
    data.finishTick = getCurrentSimCycle();
    data.cpuAvailable = managedLaunch();
    data.memoryAvailable = memoryInterface_ != nullptr;
    data.globalDMAAvailable = globalDMALink_ != nullptr;
    data.analogAvailable = analog_->enabled();
    data.networkAvailable = network_ != nullptr;
    data.scratchpadAvailable = memoryAccess_->scratchpadAvailable();
    data.timebase.sstTimebase = getCoreTimeBase().toStringBestSI(0);
    data.timebase.cpuTicksPerCycle = cpuClockTimeBase_.getFactor();
    if (data.networkAvailable)
        data.timebase.rxTicksPerCycle = getTimeConverter(config_.receiveDMAClock).getFactor();
    if (data.analogAvailable)
        data.timebase.analogTicksPerCycle = analogClockTimeBase_.getFactor();
    // NIC/router/global-RAM clocks belong to external components. Do not
    // infer their factors from the tile's network scheduling clock.
    if (!resolvedConfigurationPath_.empty())
        data.provenance.resolvedConfigReference = resolvedConfigurationPath_;
    return data;
}

void Tile::finish()
{
    if (state_ == LifecycleState::Finished)
    {
        return;
    }

    cpu_->assertDrained();
    if (analog_->hasDeferred())
    {
        output_.fatal(CALL_INFO, -1,
                      "tile %u finished with unconsumed local QEMU lookahead state\n",
                      static_cast<unsigned>(config_.tileId));
    }

    if (qemu_.running())
    {
        output_.verbose(CALL_INFO, 1, 0, "terminating QEMU tile %u during finish()\n",
                        static_cast<unsigned>(config_.tileId));
        qemu_.terminate();
    }

    if (progressWatchdogInitialized_)
    {
        recordProgressSnapshot("final");
    }

    if (config_.tileId == 0)
    {
        QemuCaptureHostStatistics& statistics = QemuCaptureCoordinator::qemuCaptureHostStatistics();
        for (std::size_t reason = 0; reason < statistics.counts.size(); ++reason)
        {
            const std::uint64_t count = statistics.counts[reason].load(std::memory_order_relaxed);
            if (count == 0)
            {
                continue;
            }
            output_.output(
                "MITTENS_QEMU_CAPTURE_HOST stop_reason=%s(%zu) "
                "count=%llu total_ns=%llu max_ns=%llu\n",
                syncStopReasonName(static_cast<std::uint32_t>(reason)), reason,
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(
                    statistics.nanoseconds[reason].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    statistics.maximumNanoseconds[reason].load(std::memory_order_relaxed)));
        }
        for (std::size_t payloadClass = 0; payloadClass < statistics.fencePayloadCounts.size();
             ++payloadClass)
        {
            const std::uint64_t count =
                statistics.fencePayloadCounts[payloadClass].load(std::memory_order_relaxed);
            if (count == 0)
            {
                continue;
            }
            output_.output("MITTENS_QEMU_FENCE_CAPTURE_HOST payload_class=%zu "
                           "memory=%u global_dma=%u analog=%u count=%llu "
                           "total_ns=%llu\n",
                           payloadClass, static_cast<unsigned>((payloadClass & 1U) != 0),
                           static_cast<unsigned>((payloadClass & 2U) != 0),
                           static_cast<unsigned>((payloadClass & 4U) != 0),
                           static_cast<unsigned long long>(count),
                           static_cast<unsigned long long>(
                               statistics.fencePayloadNanoseconds[payloadClass].load(
                                   std::memory_order_relaxed)));
        }
    }

    if (config_.qemuRuntimeReadySet)
    {
        const RuntimeQemuReadySetStatistics statistics =
            QemuCaptureCoordinator::runtimeQemuReadySetStatistics();
        if (statistics.reporterTile == config_.tileId)
        {
            output_.output("MITTENS_RUNTIME_QEMU_READY_SET "
                           "dispatches=%llu captures=%llu parallel_dispatches=%llu "
                           "max_batch=%zu workers=%u\n",
                           static_cast<unsigned long long>(statistics.dispatchCount),
                           static_cast<unsigned long long>(statistics.capturedTaskCount),
                           static_cast<unsigned long long>(statistics.parallelDispatchCount),
                           statistics.maximumBatchSize,
                           static_cast<unsigned>(config_.qemuReadySetWorkers));
        }
    }
    if (config_.qemuLocalLookahead)
    {
        const LocalQemuLookaheadStatistics statistics =
            QemuCaptureCoordinator::localQemuLookaheadStatistics();
        if (statistics.reporterTile == config_.tileId)
        {
            output_.output("MITTENS_QEMU_LOCAL_LOOKAHEAD "
                           "submitted=%llu completed=%llu ready_at_commit=%llu "
                           "waited_at_commit=%llu max_concurrent=%zu workers=%u\n",
                           static_cast<unsigned long long>(statistics.executor.submitted),
                           static_cast<unsigned long long>(statistics.executor.completed),
                           static_cast<unsigned long long>(statistics.readyAtCommit),
                           static_cast<unsigned long long>(statistics.waitedAtCommit),
                           statistics.executor.maximumConcurrent,
                           static_cast<unsigned>(statistics.workerCount));
            for (std::size_t reason = 0; reason < statistics.fusedTerminalCounts.size(); ++reason)
            {
                if (statistics.fusedTerminalCounts[reason] != 0)
                {
                    output_.output(
                        "MITTENS_QEMU_LOCAL_LOOKAHEAD_FUSED "
                        "stop_reason=%s(%zu) count=%llu\n",
                        syncStopReasonName(static_cast<std::uint32_t>(reason)), reason,
                        static_cast<unsigned long long>(statistics.fusedTerminalCounts[reason]));
                }
            }
        }
    }

    if (network_ != nullptr)
    {
        network_->finish();
    }
    if (memoryInterface_ != nullptr)
    {
        memoryInterface_->finish();
    }

    const ScratchpadTimingStatistics scratchpad = memoryAccess_->scratchpadStatistics();
    scratchpadServiceStatistic_->addData(scratchpad.activeCycles);
    scratchpadReadServiceStatistic_->addData(scratchpad.readServiceCycles);
    scratchpadWriteServiceStatistic_->addData(scratchpad.writeServiceCycles);

    cpu_->completeWait();
    analog_->flushTrace();
    // Close the last interval before emitting the per-tile report.  The
    // observer charges the interval since the most recent TX state change.
    tx_->observeTransmitOpportunity();
    const auto measurements = measurementSnapshot();
    reportTransmitOpportunity(output_, measurements);
    reportTileProfile(output_, measurements);
    try
    {
        writeTileSummary(performanceProfile_, output_, measurements);
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "tile %u could not write performance profile: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    if (rx_)
    {
        rx_->shutdown();
    }
    if (cpu_)
        cpu_->stop();
    bridge_.close();
    analog_->close();
    syncBridge_.close();
    globalRAMFileDescriptor_.reset();

    state_ = LifecycleState::Finished;
    output_.verbose(CALL_INFO, 2, 0, "tile %u finished\n", static_cast<unsigned>(config_.tileId));
}

void Tile::emergencyShutdown()
{
    if (progressWatchdogInitialized_)
    {
        try
        {
            recordProgressSnapshot("emergency");
        }
        catch (const std::exception&)
        {
            // Do not replace the originating SST fatal diagnostic. Periodic
            // checkpoints remain available if this best-effort flush fails.
        }
    }
    qemu_.terminate();
    if (rx_)
    {
        rx_->shutdown();
    }
    if (cpu_)
        cpu_->stop();
    bridge_.close();
    analog_->close();
    syncBridge_.close();
    globalRAMFileDescriptor_.reset();
}

CpuDeviceResult Tile::executeCpuNetwork(const CpuNetworkAction& action)
{
    CpuDeviceResult result;
    const auto finish = [&](bool complete)
    {
        result.complete = complete;
        return result;
    };
    switch (action.reason)
    {
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
        serviceBridge();
        /*
         * The descriptor that caused this doorbell is already committed to
         * the bridge.  Do not turn a successful submission into a wait for
         * the next queue slot.  Guest software must return to its event loop
         * so that it can consume receive work before attempting another
         * transmit.
         */
        return finish(true);

    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
        /*
         * Claim the frame before servicing the bridge. If its payload is
         * already complete, release it into the slot QEMU freed by consuming
         * the header before generic service admits a later frame header.
         */
        rx_->registerReceiveDMA({
            action.source,
            action.route,
            action.iteration,
            action.words,
            action.address,
        });
        serviceBridge();
        rx_->scheduleReceiveDMABursts();
        return finish(true);

    case MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM:
        rx_->registerReceiveSoftwareClaim({
            action.source,
            action.route,
            action.iteration,
            action.words,
        });
        serviceBridge();
        return finish(true);

    default:
        throw std::logic_error("invalid typed CPU device action");
    }
}

CpuDeviceResult Tile::executeCpuTask(const CpuTaskAction& action)
{
    const auto accounting = cpu_->accounting();
    taskTrace_.record(action.task, action.execution, action.finish, getCurrentSimCycle(),
                      accounting.total.instructions, accounting.totalCycles);
    CpuDeviceResult result;
    result.complete = true;
    return result;
}

void Tile::executeCpuExit()
{
    /*
     * A final partial TX ring may not have needed a TX_WAIT. Snapshot it
     * into SST before QEMU is reaped so every accepted burst drains.
     */
    serviceBridge();
    QemuExitStatus status;
    try
    {
        status = qemu_.waitForExit();
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1,
                      "failed to reap QEMU tile %u after its fd 41 guest-exit event: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }
    handleQemuExit(status);
}

} // namespace Mittens
} // namespace SST
