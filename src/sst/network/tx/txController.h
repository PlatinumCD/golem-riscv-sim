#ifndef SST_MITTENS_TX_CONTROLLER_H
#define SST_MITTENS_TX_CONTROLLER_H

#include "txSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>

#include "../packetEvent.h"
#include "../../bridge/sharedMemoryBridge.h"
#include "../../execution/clockDomain.h"
#include "../../memory/addressRegion.h"
#include "../../memory/scratchpad/scratchpadTimingModel.h"
#include "../../profiling/performanceProfile.h"

namespace SST::Mittens
{

// Owns TX descriptors, source FIFOs, frame ordering and accounting. The
// component retains event links, guest waits, lifecycle and shared resources.
class TxController final
{
  public:
    struct Configuration
    {
        std::uint32_t tileId;
        std::uint32_t networkSize;
        std::uint32_t meshWidth;
        std::uint32_t meshHeight;
        std::uint32_t networkPacketWords;
        std::uint32_t transmitDMAStreams;
        std::uint32_t transmitDMAFIFOBytes;
        std::uint32_t scratchpadDMABytesPerCycle;
        AddressRegion scratchpad;
        std::uint32_t deploymentFrameMagic;
        std::size_t deploymentFrameHeaderWords;
    };

    struct Resources
    {
        SharedMemoryBridge& bridge;
        SST::Interfaces::SimpleNetwork* network;
        ScratchpadTimingModel* scratchpad;
        PerformanceProfile& profile;
        SST::Output& output;
        Timing::Clock<Timing::Cpu> cpuClock;
    };

    struct Host
    {
        std::function<Timing::Ticks()> now;
        // Schedules only: must not synchronously reenter TX or resume the guest.
        // Empty when the component has no TX DMA links.
        std::function<void(std::uint32_t, Timing::Cycles<Timing::Cpu>)> scheduleDMACompletion;
        std::function<std::uint32_t(std::uint32_t, std::uint32_t)> meshHops;
    };

    using Counters = TxCounters;
    using Status = TxStatus;

    TxController(Configuration config, Resources resources, Host host);
    TxController(const TxController&) = delete;
    TxController& operator=(const TxController&) = delete;

    // True requests the existing lifecycle drain check, after service returns.
    // The caller retains bridge-error checks and guest wake ordering.
    bool service();
    // Only publishes DMA availability; caller then services TX before waking.
    // No stream denotes the legacy T1 event; T2/T4 events carry their lane.
    void onDMACompletion(std::optional<std::uint32_t> stream);
    bool idle() const noexcept;
    bool readyForGuest(bool burst) const noexcept;
    const Counters& counters() const noexcept
    {
        return counters_;
    }
    Status status() const noexcept;
    // Retains the original observation sites: service entry and final report.
    void observeTransmitOpportunity();

  private:
    struct OutgoingFrame
    {
        bool active = false;
        std::uint32_t destination = UINT32_MAX;
        std::uint32_t routeId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::uint64_t logicalIteration = UINT64_MAX;
        std::uint32_t remainingPayloadWords = 0;
    };

    // Beat/FIFO state is identical for T1 and each multi-TX lane. Scheduling
    // and frame ownership remain outside it because their policies differ.
    struct TransmitDMAState
    {
        std::optional<MittensBridgeTxBurst> burst;
        std::uint32_t offset = 0;
        std::uint32_t wordsScheduled = 0;
        std::uint32_t wordsAvailable = 0;
        std::uint32_t wordsInFlight = 0;
        bool timed = false;
        bool setupCharged = false;
    };

    struct TransmitDMAStream
    {
        TransmitDMAState dma;
        std::optional<std::uint64_t> readyTick;
        OutgoingFrame frame;
        std::optional<std::uint64_t> blockedSinceCycle;
        std::uint64_t blockedCycles = 0;
    };

    struct TransmitBlock
    {
        std::uint64_t eventSequence = 0;
        std::uint64_t startTick = 0;
        std::uint32_t routeId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::uint32_t destination = UINT32_MAX;
        std::string kind;
        bool burst = false;
        std::uint32_t words = 0;
        std::uint64_t retryCount = 0;
        std::uint32_t maximumQueueOccupancy = 0;
    };

    AddressRegion scratchpadRegion() const noexcept
    {
        return config_.scratchpad;
    }
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return resources_.cpuClock;
    }
    void serviceOutgoingPacketsMulti();
    void scheduleTransmitDMABeat();
    void scheduleTransmitDMABeat(std::size_t stream);
    void scheduleTransmitDMABeat(TransmitDMAState& dma, std::optional<std::size_t> lane);
    std::uint32_t firstHopDirection(std::uint32_t destination) const noexcept;
    void beginTransmitBlock(const PacketEvent::Metadata& metadata, std::uint32_t destination,
                            const char* kind, bool burst, std::uint32_t words,
                            std::uint32_t queueOccupancy);
    void completeTransmitBlock();
    PacketEvent::Metadata describePacket(const std::vector<std::uint32_t>& payload,
                                         std::uint32_t destination, std::uint64_t readyTick,
                                         std::uint64_t injectionTick,
                                         OutgoingFrame& nextFrame) const;

    const Configuration config_;
    Resources resources_;
    Host host_;
    Counters counters_;
    std::vector<TransmitDMAStream> transmitDMAStreams_;
    // Interval-accounted fan-out opportunity counters.  A "ready" transfer
    // includes the burst currently owned by TX DMA plus bursts queued by the
    // guest.  This deliberately measures frontend serialization before the
    // router is consulted.
    std::optional<std::uint64_t> transmitOpportunityLastTick_;
    std::uint32_t transmitOpportunityReadyTransfers_ = 0;
    std::uint32_t transmitOpportunityDirections_ = 0;
    std::uint32_t transmitOpportunityActiveDirections_ = 0;
    std::uint32_t transmitOpportunityActiveLanes_ = 0;
    std::uint32_t transmitOpportunityQueuedTransfers_ = 0;
    std::uint64_t transmitOpportunityQueuedBytes_ = 0;
    std::uint32_t transmitOpportunityFIFOEmptyLanes_ = 0;
    std::uint32_t transmitOpportunityFIFOFullLanes_ = 0;
    std::optional<TransmitBlock> activeTransmitBlock_;
    std::uint64_t nextTransmitBlockSequence_ = 1;
    std::uint64_t nextNetworkPacketId_ = 1;
    std::optional<std::uint64_t> pendingTransmitReadyTick_;
    std::optional<std::uint64_t> pendingTransmitBurstReadyTick_;
    OutgoingFrame outgoingFrame_;

    std::optional<MittensBridgePacket> pendingTransmit_;
    TransmitDMAState singleTransmitDMA_;
};

} // namespace SST::Mittens

#endif
