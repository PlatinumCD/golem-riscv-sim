#ifndef SST_MITTENS_RX_CONTROLLER_H
#define SST_MITTENS_RX_CONTROLLER_H

#include "rxSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>

#include "../packetEvent.h"
#include "../receiveDMAEngine.h"
#include "../../bridge/sharedMemoryBridge.h"
#include "../../execution/clockDomain.h"
#include "../../memory/addressRegion.h"
#include "../../memory/scratchpad/scratchpadTimingModel.h"
#include "../../profiling/performanceProfile.h"

namespace SST::Mittens
{

// Owns RX protocol state and ordinary-RAM DMA timing. Shared resources remain
// owned by the component; no callback grants access to component/guest state.
class RxController final
{
  public:
    struct Configuration
    {
        std::uint32_t tileId;
        std::uint32_t networkSize;
        std::uint32_t receiveDMAQueueDepth;
        std::uint32_t receiveDMAWidthBits;
        std::uint64_t receiveDMASetupCycles;
        bool scratchpadEnabled;
        std::uint64_t scratchpadBytes;
        std::uint64_t memoryGuestBase;
        std::uint64_t memoryTileStride;
        std::uint32_t memoryCacheLineSize;
        bool networkTailDelivery;
        std::uint32_t meshLinkWidthBits;
        std::uint32_t deploymentFrameMagic;
        std::size_t deploymentFrameHeaderWords;
        bool receiveDMAStreaming = false;
        std::uint32_t receiveDMAStreams = 1;
    };

    struct Resources
    {
        SharedMemoryBridge& bridge;
        SST::Interfaces::SimpleNetwork* network;
        SST::Interfaces::StandardMem* memory;
        ScratchpadTimingModel* scratchpad;
        PerformanceProfile& profile;
        SST::Output& output;
        Timing::Clock<Timing::Cpu> cpuClock;
        Timing::Clock<Timing::ReceiveDMA> dmaClock;
        Timing::Clock<Timing::Network> networkClock;
    };

    struct Host
    {
        std::function<Timing::Ticks()> now;
        // These schedule self-events only; they must not synchronously reenter
        // the controller. Tile owns and dispatches the SST links/events.
        std::function<void(Timing::Cycles<Timing::Cpu>)> scheduleDMACompletion;
        std::function<void(Timing::Cycles<Timing::Network>)> scheduleNetworkCompletion;
        std::function<std::uint32_t(std::uint32_t, std::uint32_t)> meshHops;
    };

    struct ReceiveClaim
    {
        std::uint32_t source;
        std::uint32_t routeId;
        std::uint64_t logicalIteration;
        std::uint32_t wordCount;
        std::uint64_t destination = 0;
    };

    using Counters = RxCounters;
    using Status = RxStatus;

    enum class InvalidationResult
    {
        Unhandled,
        Pending,
        Authorized
    };

    RxController(Configuration config, Resources resources, Host host);
    RxController(const RxController&) = delete;
    RxController& operator=(const RxController&) = delete;

    void service();
    void onNetworkCompletion();
    // True means authorization was published. The caller then checks RX waits
    // followed by TX waits, after all RX references have left scope.
    bool onDMACompletion();
    // Takes ownership of request only when the result is not Unhandled.
    InvalidationResult handleInvalidationResponse(SST::Interfaces::StandardMem::Request* request);
    void registerReceiveDMA(const ReceiveClaim& claim);
    void registerReceiveSoftwareClaim(const ReceiveClaim& claim);
    void scheduleReceiveDMABursts();
    bool readyForGuest() const noexcept;
    const Counters& counters() const noexcept
    {
        return counters_;
    }
    Status status() const;

    // Terminal only: not guest exit, which may still service RX while TX drains.
    // Freeze state until destruction and suppress new work/authorization. Late
    // invalidation replies are still recognized and consumed.
    void shutdown() noexcept
    {
        stopped_ = true;
    }

  private:
    struct ReceiveDMADescriptor
    {
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint64_t logicalIteration;
        std::uint64_t destination;
        std::uint32_t remainingWords;
        bool setupCharged;
    };

    struct ReceiveDMATransfer
    {
        std::uint32_t burstIndex;
        std::uint32_t source;
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint64_t logicalIteration;
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

    struct PendingNetworkReceive
    {
        std::uint32_t source;
        PacketEvent::Metadata metadata;
        std::vector<std::uint32_t> payload;
        std::uint64_t completionTick;
    };

    enum class ReceivePayloadOwner : std::uint8_t
    {
        Unclaimed,
        Software,
        DMA,
    };

    struct IncomingFrameAssembly
    {
        std::uint32_t routeId;
        std::uint64_t executionId;
        std::uint64_t logicalIteration;
        std::uint32_t expectedWords;
        std::vector<std::uint32_t> header;
        std::vector<std::uint32_t> payload;
        bool headerExposed;
        bool payloadQueued;
        std::uint32_t finalPayloadBurstIndex;
        bool finalPayloadBurstPublished;
        ReceivePayloadOwner payloadOwner;
        std::size_t queuedWords = 0;
    };

    struct ReadyReceiveBurst
    {
        std::uint32_t source;
        std::vector<std::uint32_t> payload;
        bool softwareVisible;
        std::uint32_t routeId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::uint64_t logicalIteration = UINT64_MAX;
        bool finalFramePayload = false;
    };

    struct IncomingRouteTag
    {
        std::uint64_t executionId;
        std::uint64_t logicalIteration;
    };

    AddressRegion scratchpadRegion() const noexcept;
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return resources_.cpuClock;
    }
    Timing::Clock<Timing::ReceiveDMA> receiveDMADomain() const
    {
        return resources_.dmaClock;
    }
    Timing::Clock<Timing::Network> networkDomain() const
    {
        return resources_.networkClock;
    }
    void serviceIncomingPackets();
    void completeReadyNetworkReceives();
    void completeNetworkReceive(PendingNetworkReceive receive);
    void exposeNextReceiveHeader(std::uint32_t source);
    void releaseCompletedReceiveFrames(std::uint32_t source);
    void retireConsumedReceiveFrames(std::uint32_t source);
    void flushReadyReceiveBursts();
    IncomingFrameAssembly* findReceiveFrame(std::uint32_t source, std::uint32_t routeId,
                                            std::uint64_t executionId,
                                            std::uint64_t logicalIteration) noexcept;
    // Call only after the mode-specific descriptor/range checks. Consumes one
    // route tag and claims one matching frame; callers retain queue ordering.
    std::uint64_t claimReceiveFrame(const ReceiveClaim& event, ReceivePayloadOwner owner);
    void refreshReceiveDMATransfers();
    bool receiveBurstScheduled(std::uint32_t burstIndex) const noexcept;
    void authorizeReceiveDMA(ReceiveDMATransfer& transfer);
    bool takeRouteExecutionId(std::uint32_t source, std::uint32_t routeId,
                              std::uint64_t logicalIteration, std::uint64_t* executionId) noexcept;

    const Configuration config_;
    Resources resources_;
    Host host_;
    std::vector<ReceiveDMAEngine> receiveDMAEngines_;
    std::vector<std::uint64_t> receiveLaneAvailable_;
    std::unordered_map<std::uint32_t, std::uint64_t> receiveSourceAvailable_;
    Counters counters_;
    bool stopped_ = false;
    std::uint64_t networkReceiveNextAvailableTick_ = 0;
    std::unordered_map<std::uint64_t, std::deque<IncomingRouteTag>> incomingRouteExecutions_;
    std::unordered_map<std::uint32_t, std::deque<ReceiveDMADescriptor>> receiveDMADescriptors_;
    std::unordered_map<SST::Interfaces::StandardMem::Request::id_t, std::uint32_t>
        receiveDMAInvalidations_;
    std::deque<PendingNetworkReceive> pendingNetworkReceives_;
    std::unordered_map<std::uint32_t, IncomingFrameAssembly> incomingFrameAssemblies_;
    std::unordered_map<std::uint32_t, std::deque<IncomingFrameAssembly>> completedReceiveFrames_;
    std::size_t completedReceiveFrameCount_ = 0;
    std::deque<ReadyReceiveBurst> readyReceiveBursts_;
    std::deque<ReceiveDMATransfer> receiveDMATransfersInFlight_;
};

} // namespace SST::Mittens

#endif
