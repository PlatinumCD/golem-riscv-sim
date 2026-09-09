#include "sst_config.h"
#include "txController.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace SST::Mittens
{

TxController::TxController(Configuration config, Resources resources, Host host)
    : config_(config), resources_(resources), host_(std::move(host))
{
    if (host_.scheduleDMACompletion && config_.transmitDMAStreams > 1)
    {
        transmitDMAStreams_.resize(config_.transmitDMAStreams);
    }
}

void TxController::onDMACompletion(std::optional<std::uint32_t> lane)
{
    if (lane.has_value() && *lane >= transmitDMAStreams_.size())
    {
        resources_.output.fatal(
            CALL_INFO, -1, "tile %u received invalid TX stream completion %u\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(*lane));
    }
    TransmitDMAState& dma = lane.has_value() ? transmitDMAStreams_[*lane].dma
                                            : singleTransmitDMA_;
    if (!resources_.bridge.open() || !dma.burst.has_value() || !dma.timed ||
        dma.wordsInFlight == 0)
    {
        resources_.output.fatal(CALL_INFO, -1,
            lane.has_value() ? "tile %u received invalid TX DMA stream completion\n"
                            : "tile %u received an invalid TX DMA completion\n",
            static_cast<unsigned>(config_.tileId));
    }
    if (dma.wordsInFlight > dma.burst->word_count - dma.wordsAvailable)
    {
        resources_.output.fatal(CALL_INFO, -1,
            lane.has_value() ? "tile %u received invalid TX DMA stream completion\n"
                            : "tile %u TX DMA completion exceeds its burst\n",
            static_cast<unsigned>(config_.tileId));
    }
    dma.wordsAvailable += dma.wordsInFlight;
    dma.wordsInFlight = 0;
}

void TxController::scheduleTransmitDMABeat()
{
    scheduleTransmitDMABeat(singleTransmitDMA_, std::nullopt);
}

void TxController::scheduleTransmitDMABeat(std::size_t index)
{
    scheduleTransmitDMABeat(transmitDMAStreams_[index].dma, index);
}

void TxController::scheduleTransmitDMABeat(TransmitDMAState& dma,
                                          std::optional<std::size_t> lane)
{
    if (!dma.burst.has_value() || !dma.timed || dma.wordsInFlight != 0 ||
        resources_.scratchpad == nullptr || !host_.scheduleDMACompletion ||
        dma.wordsScheduled >= dma.burst->word_count)
    {
        return;
    }
    const std::uint32_t fifoWords = config_.transmitDMAFIFOBytes / sizeof(std::uint32_t);
    const std::uint32_t resident = dma.wordsAvailable - dma.offset;
    if (resident >= fifoWords)
        return;
    const std::uint32_t beatWords = config_.scratchpadDMABytesPerCycle / sizeof(std::uint32_t);
    const std::uint32_t words = std::min(
        {beatWords, dma.burst->word_count - dma.wordsScheduled, fifoWords - resident});
    const std::uint64_t offset =
        scratchpadRegion().offset(dma.burst->source_address) +
        static_cast<std::uint64_t>(dma.wordsScheduled) * sizeof(std::uint32_t);
    const std::uint64_t current = cpuDomain().floor(host_.now()).value;
    ScratchpadSchedule timing;
    try
    {
        const auto client = static_cast<ScratchpadDMAClient>(
            static_cast<std::uint8_t>(ScratchpadDMAClient::NetworkTransmit) + lane.value_or(0));
        timing = resources_.scratchpad->scheduleDMA(
            current, offset, static_cast<std::uint64_t>(words) * sizeof(std::uint32_t), false,
            client, !dma.setupCharged);
    }
    catch (const std::exception& error)
    {
        if (lane.has_value())
        {
            resources_.output.fatal(CALL_INFO, -1, "tile %u cannot schedule TX DMA stream %zu: %s\n",
                                    static_cast<unsigned>(config_.tileId), *lane, error.what());
        }
        resources_.output.fatal(CALL_INFO, -1, "tile %u cannot schedule TX DMA: %s\n",
                                static_cast<unsigned>(config_.tileId), error.what());
    }
    dma.setupCharged = true;
    counters_.transmitDMAActiveCycles += timing.serviceCycles;
    dma.wordsScheduled += words;
    dma.wordsInFlight = words;
    host_.scheduleDMACompletion(static_cast<std::uint32_t>(lane.value_or(0)),
                                Timing::Cycles<Timing::Cpu>{timing.completionCycle - current});
}

bool TxController::service()
{
    observeTransmitOpportunity();
    if (config_.transmitDMAStreams > 1)
    {
        serviceOutgoingPacketsMulti();
        return true;
    }
    constexpr int kVirtualNetwork = 0;
    constexpr int kPacketBits = 32;

    while (true)
    {
        /*
         * A blocked scalar/header packet must be retried before a queued
         * burst. Otherwise a TX-DMA wake can try a later payload while the
         * pending block still describes the header, corrupting the block
         * accounting and incorrectly reporting that the blocked packet
         * changed.
         */
        const bool retryBlockedScalarPacket =
            activeTransmitBlock_.has_value() && !activeTransmitBlock_->burst;
        if (!retryBlockedScalarPacket)
        {
            if (!singleTransmitDMA_.burst.has_value())
            {
                singleTransmitDMA_.burst = resources_.bridge.popTransmitBurst();
                if (singleTransmitDMA_.burst.has_value())
                {
                    singleTransmitDMA_.offset = 0;
                    singleTransmitDMA_.wordsScheduled = 0;
                    singleTransmitDMA_.wordsAvailable = 0;
                    singleTransmitDMA_.wordsInFlight = 0;
                    singleTransmitDMA_.setupCharged = false;
                    const std::uint64_t sourceAddress = singleTransmitDMA_.burst->source_address;
                    singleTransmitDMA_.timed =
                        config_.transmitDMAFIFOBytes != 0 &&
                        scratchpadRegion().containsRange(
                            sourceAddress,
                            static_cast<std::uint64_t>(singleTransmitDMA_.burst->word_count) *
                                sizeof(std::uint32_t));
                    if (!singleTransmitDMA_.timed)
                    {
                        singleTransmitDMA_.wordsScheduled = singleTransmitDMA_.burst->word_count;
                        singleTransmitDMA_.wordsAvailable = singleTransmitDMA_.burst->word_count;
                    }
                    pendingTransmitBurstReadyTick_ = host_.now().value;
                }
            }
            if (singleTransmitDMA_.burst.has_value())
            {
                const MittensBridgeTxBurst& burst = *singleTransmitDMA_.burst;
                if (burst.destination >= config_.networkSize)
                {
                    resources_.output.fatal(CALL_INFO, -1,
                                            "tile %u attempted to send a burst to invalid "
                                            "destination %u\n",
                                            static_cast<unsigned>(config_.tileId),
                                            static_cast<unsigned>(burst.destination));
                }
                if (burst.word_count == 0 || burst.word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY)
                {
                    resources_.output.fatal(CALL_INFO, -1,
                                            "tile %u submitted invalid %u-word burst\n",
                                            static_cast<unsigned>(config_.tileId),
                                            static_cast<unsigned>(burst.word_count));
                }

                scheduleTransmitDMABeat();
                const std::uint32_t availableWords =
                    singleTransmitDMA_.wordsAvailable - singleTransmitDMA_.offset;
                if (availableWords == 0)
                {
                    return false;
                }
                const std::uint32_t remainingWords =
                    std::min(burst.word_count - singleTransmitDMA_.offset, availableWords);
                const std::uint32_t packetWords =
                    std::min(remainingWords, config_.networkPacketWords);
                const int packetBits = static_cast<int>(packetWords) * kPacketBits;
                const std::uint32_t* const packetBegin = burst.words + singleTransmitDMA_.offset;
                if (!resources_.network->spaceToSend(kVirtualNetwork, packetBits))
                {
                    const std::uint64_t blockTick = host_.now().value;
                    const std::vector<std::uint32_t> blockedPayload(packetBegin,
                                                                    packetBegin + packetWords);
                    OutgoingFrame blockedFrame = outgoingFrame_;
                    const PacketEvent::Metadata blockedMetadata =
                        describePacket(blockedPayload, burst.destination,
                                       pendingTransmitBurstReadyTick_.value_or(blockTick),
                                       blockTick, blockedFrame);
                    beginTransmitBlock(
                        blockedMetadata, burst.destination,
                        blockedMetadata.protocolWords != 0
                            ? "frame-header"
                            : (blockedMetadata.payloadWords != 0 ? "frame-payload" : "raw"),
                        true, packetWords, 1U + resources_.bridge.transmitBurstCount());
                    return false;
                }

                std::vector<std::uint32_t> payload(packetBegin, packetBegin + packetWords);
                const std::uint64_t injectionTick = host_.now().value;
                completeTransmitBlock();
                OutgoingFrame nextFrame = outgoingFrame_;
                PacketEvent::Metadata metadata =
                    describePacket(payload, burst.destination,
                                   pendingTransmitBurstReadyTick_.value_or(injectionTick),
                                   injectionTick, nextFrame);
                auto* request = new SST::Interfaces::SimpleNetwork::Request(
                    burst.destination, config_.tileId, packetBits, true, true,
                    new PacketEvent(std::move(payload), metadata));
                if (!resources_.network->send(request, kVirtualNetwork))
                {
                    delete request;
                    return false;
                }
                outgoingFrame_ = nextFrame;
                ++nextNetworkPacketId_;
                ++counters_.networkTransmitPackets;
                counters_.networkTransmitWords += packetWords;
                const std::uint32_t hops = host_.meshHops(config_.tileId, burst.destination);
                counters_.networkWordHops += static_cast<std::uint64_t>(packetWords) * hops;
                counters_.networkEndpointQueueTicks +=
                    injectionTick >= metadata.readyTick ? injectionTick - metadata.readyTick : 0;
                resources_.profile.recordNetwork(
                    "inject", metadata.packetId, config_.tileId, burst.destination,
                    metadata.routeId, metadata.executionId, metadata.logicalIteration,
                    metadata.protocolWords != 0
                        ? "frame-header"
                        : (metadata.payloadWords != 0 ? "frame-payload" : "raw"),
                    packetWords, metadata.protocolWords, metadata.payloadWords, hops,
                    metadata.readyTick, metadata.injectionTick, injectionTick);

                resources_.output.verbose(
                    CALL_INFO, 2, 0,
                    "tile %u sent %u-word packet to tile %u "
                    "(%u/%u burst words)\n",
                    static_cast<unsigned>(config_.tileId), static_cast<unsigned>(packetWords),
                    static_cast<unsigned>(burst.destination),
                    static_cast<unsigned>(singleTransmitDMA_.offset + packetWords),
                    static_cast<unsigned>(burst.word_count));
                singleTransmitDMA_.offset += packetWords;
                if (singleTransmitDMA_.offset == burst.word_count)
                {
                    if (singleTransmitDMA_.wordsInFlight != 0 ||
                        singleTransmitDMA_.wordsAvailable != burst.word_count ||
                        singleTransmitDMA_.wordsScheduled != burst.word_count)
                    {
                        resources_.output.fatal(
                            CALL_INFO, -1,
                            "tile %u completed a TX burst with incomplete DMA state\n",
                            static_cast<unsigned>(config_.tileId));
                    }
                    singleTransmitDMA_.burst.reset();
                    singleTransmitDMA_.offset = 0;
                    singleTransmitDMA_.wordsScheduled = 0;
                    singleTransmitDMA_.wordsAvailable = 0;
                    singleTransmitDMA_.wordsInFlight = 0;
                    singleTransmitDMA_.timed = false;
                    singleTransmitDMA_.setupCharged = false;
                    pendingTransmitBurstReadyTick_.reset();
                }
                continue;
            }
        }

        if (!pendingTransmit_.has_value())
        {
            pendingTransmit_ = resources_.bridge.popTransmit();
            if (pendingTransmit_.has_value())
            {
                pendingTransmitReadyTick_ = host_.now().value;
            }
        }
        if (!pendingTransmit_.has_value())
        {
            return true;
        }

        if (pendingTransmit_->destination >= config_.networkSize)
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u attempted to send to invalid destination %u\n",
                                    static_cast<unsigned>(config_.tileId),
                                    static_cast<unsigned>(pendingTransmit_->destination));
        }

        if (!resources_.network->spaceToSend(kVirtualNetwork, kPacketBits))
        {
            const std::uint64_t blockTick = host_.now().value;
            const std::vector<std::uint32_t> blockedPayload{pendingTransmit_->payload};
            OutgoingFrame blockedFrame = outgoingFrame_;
            const PacketEvent::Metadata blockedMetadata = describePacket(
                blockedPayload, pendingTransmit_->destination,
                pendingTransmitReadyTick_.value_or(blockTick), blockTick, blockedFrame);
            beginTransmitBlock(blockedMetadata, pendingTransmit_->destination,
                               blockedMetadata.protocolWords != 0
                                   ? "frame-header"
                                   : (blockedMetadata.payloadWords != 0 ? "frame-payload" : "raw"),
                               false, 1, 1U + resources_.bridge.transmitCount());
            return false;
        }

        const std::uint64_t injectionTick = host_.now().value;
        completeTransmitBlock();
        std::vector<std::uint32_t> payload{pendingTransmit_->payload};
        OutgoingFrame nextFrame = outgoingFrame_;
        PacketEvent::Metadata metadata = describePacket(
            payload, pendingTransmit_->destination,
            pendingTransmitReadyTick_.value_or(injectionTick), injectionTick, nextFrame);
        auto* request = new SST::Interfaces::SimpleNetwork::Request(
            pendingTransmit_->destination, config_.tileId, kPacketBits, true, true,
            new PacketEvent(std::move(payload), metadata));

        if (!resources_.network->send(request, kVirtualNetwork))
        {
            delete request;
            return false;
        }
        outgoingFrame_ = nextFrame;
        ++nextNetworkPacketId_;
        ++counters_.networkTransmitPackets;
        ++counters_.networkTransmitWords;
        const std::uint32_t hops = host_.meshHops(config_.tileId, pendingTransmit_->destination);
        counters_.networkWordHops += hops;
        counters_.networkEndpointQueueTicks +=
            injectionTick >= metadata.readyTick ? injectionTick - metadata.readyTick : 0;
        resources_.profile.recordNetwork(
            "inject", metadata.packetId, config_.tileId, pendingTransmit_->destination,
            metadata.routeId, metadata.executionId, metadata.logicalIteration,
            metadata.protocolWords != 0 ? "frame-header"
                                        : (metadata.payloadWords != 0 ? "frame-payload" : "raw"),
            1, metadata.protocolWords, metadata.payloadWords, hops, metadata.readyTick,
            metadata.injectionTick, injectionTick);

        resources_.output.verbose(CALL_INFO, 2, 0, "tile %u sent payload 0x%08x to tile %u\n",
                                  static_cast<unsigned>(config_.tileId),
                                  static_cast<unsigned>(pendingTransmit_->payload),
                                  static_cast<unsigned>(pendingTransmit_->destination));
        pendingTransmit_.reset();
        pendingTransmitReadyTick_.reset();
    }
}

void TxController::serviceOutgoingPacketsMulti()
{
    constexpr int kVirtualNetwork = 0;
    constexpr int kWordBits = 32;
    bool progressed = true;
    while (progressed)
    {
        progressed = false;

        // Preserve each deployment frame on one lane. A header opens the
        // lane's frame state; its payload is routed back to that lane even if
        // another lane becomes idle first.
        while (resources_.bridge.transmitBurstCount() != 0)
        {
            const auto front = resources_.bridge.peekTransmitBurst();
            if (!front.has_value())
                break;
            std::optional<std::size_t> selected;
            bool destinationOwned = false;
            for (std::size_t index = 0; index < transmitDMAStreams_.size(); ++index)
            {
                const auto& stream = transmitDMAStreams_[index];
                const bool activeBurstToDestination =
                    stream.dma.burst.has_value() && stream.dma.burst->destination == front->destination;
                const bool openFrameToDestination =
                    stream.frame.active && stream.frame.destination == front->destination;
                if (activeBurstToDestination || openFrameToDestination)
                {
                    destinationOwned = true;
                    if (!stream.dma.burst.has_value())
                        selected = index;
                    break;
                }
            }
            // The payload descriptor directly behind an in-flight header must
            // wait for that header's lane. Otherwise one software frame can
            // be split across local injection lanes and reordered.
            if (!selected.has_value() && destinationOwned)
                break;
            if (!selected.has_value())
            {
                for (std::size_t index = 0; index < transmitDMAStreams_.size(); ++index)
                {
                    const auto& stream = transmitDMAStreams_[index];
                    if (!stream.dma.burst.has_value() && !stream.frame.active)
                    {
                        selected = index;
                        break;
                    }
                }
            }
            if (!selected.has_value())
                break;
            auto burst = resources_.bridge.popTransmitBurst();
            if (!burst.has_value())
                break;
            TransmitDMAStream& stream = transmitDMAStreams_[*selected];
            stream.dma.burst = std::move(burst);
            stream.dma.offset = 0;
            stream.dma.wordsScheduled = 0;
            stream.dma.wordsAvailable = 0;
            stream.dma.wordsInFlight = 0;
            stream.dma.setupCharged = false;
            stream.readyTick = host_.now().value;
            const std::uint64_t source = stream.dma.burst->source_address;
            stream.dma.timed = config_.transmitDMAFIFOBytes != 0 &&
                           scratchpadRegion().containsRange(
                               source, static_cast<std::uint64_t>(stream.dma.burst->word_count) *
                                           sizeof(std::uint32_t));
            if (!stream.dma.timed)
            {
                stream.dma.wordsScheduled = stream.dma.burst->word_count;
                stream.dma.wordsAvailable = stream.dma.burst->word_count;
            }
            progressed = true;
        }

        for (std::size_t index = 0; index < transmitDMAStreams_.size(); ++index)
        {
            TransmitDMAStream& stream = transmitDMAStreams_[index];
            if (!stream.dma.burst.has_value())
                continue;
            MittensBridgeTxBurst& burst = *stream.dma.burst;
            if (burst.destination >= config_.networkSize || burst.word_count == 0 ||
                burst.word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY)
            {
                resources_.output.fatal(CALL_INFO, -1,
                                        "tile %u has invalid burst on TX stream %zu\n",
                                        static_cast<unsigned>(config_.tileId), index);
            }
            scheduleTransmitDMABeat(index);
            const std::uint32_t available = stream.dma.wordsAvailable - stream.dma.offset;
            if (available == 0)
                continue;
            const std::uint32_t words =
                std::min({burst.word_count - stream.dma.offset, available, config_.networkPacketWords});
            const int bits = static_cast<int>(words) * kWordBits;
            if (!resources_.network->spaceToSend(kVirtualNetwork, bits))
            {
                if (!stream.blockedSinceCycle.has_value())
                {
                    stream.blockedSinceCycle = cpuDomain().floor(host_.now()).value;
                }
                continue;
            }
            const std::uint64_t injectionTick = host_.now().value;
            if (stream.blockedSinceCycle.has_value())
            {
                const std::uint64_t current = cpuDomain().floor(host_.now()).value;
                stream.blockedCycles += current - *stream.blockedSinceCycle;
                stream.blockedSinceCycle.reset();
            }
            std::vector<std::uint32_t> payload(burst.words + stream.dma.offset,
                                               burst.words + stream.dma.offset + words);
            OutgoingFrame nextFrame = stream.frame;
            PacketEvent::Metadata metadata =
                describePacket(payload, burst.destination, stream.readyTick.value_or(injectionTick),
                               injectionTick, nextFrame);
            metadata.injectionLane = static_cast<std::uint32_t>(index);
            auto* request = new SST::Interfaces::SimpleNetwork::Request(
                burst.destination, config_.tileId, bits, true, true,
                new PacketEvent(std::move(payload), metadata));
            if (!resources_.network->send(request, kVirtualNetwork))
            {
                delete request;
                continue;
            }
            stream.frame = nextFrame;
            ++nextNetworkPacketId_;
            ++counters_.networkTransmitPackets;
            counters_.networkTransmitWords += words;
            const std::uint32_t hops = host_.meshHops(config_.tileId, burst.destination);
            counters_.networkWordHops += static_cast<std::uint64_t>(words) * hops;
            counters_.networkEndpointQueueTicks +=
                injectionTick >= metadata.readyTick ? injectionTick - metadata.readyTick : 0;
            resources_.profile.recordNetwork(
                "inject", metadata.packetId, config_.tileId, burst.destination, metadata.routeId,
                metadata.executionId, metadata.logicalIteration,
                metadata.protocolWords != 0
                    ? "frame-header"
                    : (metadata.payloadWords != 0 ? "frame-payload" : "raw"),
                words, metadata.protocolWords, metadata.payloadWords, hops, metadata.readyTick,
                metadata.injectionTick, injectionTick);
            stream.dma.offset += words;
            progressed = true;
            if (stream.dma.offset == burst.word_count)
            {
                if (stream.dma.wordsInFlight != 0 || stream.dma.wordsAvailable != burst.word_count ||
                    stream.dma.wordsScheduled != burst.word_count)
                {
                    resources_.output.fatal(
                        CALL_INFO, -1,
                        "tile %u completed TX stream %zu with incomplete DMA state\n",
                        static_cast<unsigned>(config_.tileId), index);
                }
                stream.dma.burst.reset();
                stream.dma.offset = stream.dma.wordsScheduled = stream.dma.wordsAvailable =
                    stream.dma.wordsInFlight = 0;
                stream.dma.timed = stream.dma.setupCharged = false;
                stream.readyTick.reset();
            }
        }
    }

    // Scalar control traffic (acknowledgements, doorbells) remains one-word
    // packets and can use any available local lane selected by the NIC.
    while (true)
    {
        if (!pendingTransmit_.has_value())
        {
            pendingTransmit_ = resources_.bridge.popTransmit();
            if (pendingTransmit_.has_value())
                pendingTransmitReadyTick_ = host_.now().value;
        }
        if (!pendingTransmit_.has_value())
            break;
        if (!resources_.network->spaceToSend(kVirtualNetwork, kWordBits))
            break;
        const std::uint64_t tick = host_.now().value;
        std::vector<std::uint32_t> payload{pendingTransmit_->payload};
        OutgoingFrame next = outgoingFrame_;
        PacketEvent::Metadata metadata =
            describePacket(payload, pendingTransmit_->destination,
                           pendingTransmitReadyTick_.value_or(tick), tick, next);
        auto* request = new SST::Interfaces::SimpleNetwork::Request(
            pendingTransmit_->destination, config_.tileId, kWordBits, true, true,
            new PacketEvent(std::move(payload), metadata));
        if (!resources_.network->send(request, kVirtualNetwork))
        {
            delete request;
            break;
        }
        outgoingFrame_ = next;
        ++nextNetworkPacketId_;
        ++counters_.networkTransmitPackets;
        ++counters_.networkTransmitWords;
        counters_.networkWordHops += host_.meshHops(config_.tileId, pendingTransmit_->destination);
        pendingTransmit_.reset();
        pendingTransmitReadyTick_.reset();
    }
}

void TxController::observeTransmitOpportunity()
{
    const std::uint64_t tick = cpuDomain().floor(host_.now()).value;
    if (transmitOpportunityLastTick_.has_value())
    {
        const std::uint64_t elapsed = tick - *transmitOpportunityLastTick_;
        const std::uint32_t bucket = std::min(transmitOpportunityReadyTransfers_, 4U);
        counters_.transmitReadyCycles[bucket] += elapsed;
        counters_.transmitActiveLaneCycles[std::min(transmitOpportunityActiveLanes_, 4U)] +=
            elapsed;
        counters_.transmitReadyDirectionCycles[std::min(transmitOpportunityDirections_, 4U)] +=
            elapsed;
        counters_.transmitQueueOccupancyCycleSum += elapsed * transmitOpportunityQueuedTransfers_;
        counters_.transmitOpportunityObservedCycles += elapsed;
        if (transmitOpportunityFIFOEmptyLanes_ != 0)
        {
            counters_.transmitFIFOEmptyCycles += elapsed;
            counters_.transmitFIFOEmptyLaneCycles += elapsed * transmitOpportunityFIFOEmptyLanes_;
        }
        if (transmitOpportunityFIFOFullLanes_ != 0)
        {
            counters_.transmitFIFOFullCycles += elapsed;
            counters_.transmitFIFOFullLaneCycles += elapsed * transmitOpportunityFIFOFullLanes_;
        }
        if (transmitOpportunityReadyTransfers_ >= 2)
        {
            if (transmitOpportunityDirections_ >= 2)
            {
                counters_.transmitIndependentReadyCycles += elapsed;
            }
            else
            {
                counters_.transmitSameDirectionReadyCycles += elapsed;
            }
        }
        if (transmitOpportunityActiveLanes_ >= 2 && transmitOpportunityActiveDirections_ >= 2)
        {
            counters_.transmitMultipleActiveIndependentCycles += elapsed;
        }
        // A multi-ready interval is an opportunity, not necessarily a stall.
        // Charge frontend serialization only when demand exceeds the number of
        // independently schedulable TX lanes in this architectural variant.
        if (transmitOpportunityReadyTransfers_ > config_.transmitDMAStreams)
        {
            counters_.transmitSerializationStallCycles += elapsed;
            counters_.transmitSerializationByteCycles += elapsed * transmitOpportunityQueuedBytes_;
        }
        if (transmitOpportunityDirections_ > config_.transmitDMAStreams)
        {
            counters_.transmitIndependentSerializationStallCycles += elapsed;
        }
    }

    std::array<bool, 5> directions{};
    std::array<bool, 5> activeDirections{};
    std::uint32_t ready = 0;
    std::uint32_t activeLanes = 0;
    std::uint32_t fifoEmptyLanes = 0;
    std::uint32_t fifoFullLanes = 0;
    std::uint64_t queuedBytes = 0;
    if (!transmitDMAStreams_.empty())
    {
        for (const TransmitDMAStream& stream : transmitDMAStreams_)
        {
            if (!stream.dma.burst.has_value())
                continue;
            ++activeLanes;
            ++ready;
            const std::uint32_t direction = firstHopDirection(stream.dma.burst->destination);
            directions[direction] = true;
            activeDirections[direction] = true;
            if (stream.dma.timed)
            {
                const std::uint32_t resident = stream.dma.wordsAvailable - stream.dma.offset;
                const std::uint32_t fifoWords =
                    config_.transmitDMAFIFOBytes / sizeof(std::uint32_t);
                if (resident == 0 && stream.dma.offset < stream.dma.burst->word_count)
                {
                    ++fifoEmptyLanes;
                }
                if (resident >= fifoWords && stream.dma.wordsScheduled < stream.dma.burst->word_count)
                {
                    ++fifoFullLanes;
                }
            }
        }
    }
    else if (singleTransmitDMA_.burst.has_value())
    {
        activeLanes = 1;
        ++ready;
        directions[firstHopDirection(singleTransmitDMA_.burst->destination)] = true;
        activeDirections[firstHopDirection(singleTransmitDMA_.burst->destination)] = true;
        if (singleTransmitDMA_.timed)
        {
            const std::uint32_t resident =
                singleTransmitDMA_.wordsAvailable - singleTransmitDMA_.offset;
            const std::uint32_t fifoWords = config_.transmitDMAFIFOBytes / sizeof(std::uint32_t);
            if (resident == 0 && singleTransmitDMA_.offset < singleTransmitDMA_.burst->word_count)
            {
                fifoEmptyLanes = 1;
            }
            if (resident >= fifoWords &&
                singleTransmitDMA_.wordsScheduled < singleTransmitDMA_.burst->word_count)
            {
                fifoFullLanes = 1;
            }
        }
    }
    const std::uint32_t queued = resources_.bridge.transmitBurstCount();
    for (std::uint32_t offset = 0; offset < queued; ++offset)
    {
        const std::optional<MittensBridgeTxBurst> burst =
            resources_.bridge.peekTransmitBurst(offset);
        if (!burst.has_value())
        {
            break;
        }
        ++ready;
        directions[firstHopDirection(burst->destination)] = true;
        queuedBytes += static_cast<std::uint64_t>(burst->word_count) * sizeof(std::uint32_t);
    }
    std::uint32_t directionCount = 0;
    std::uint32_t activeDirectionCount = 0;
    for (const bool used : directions)
    {
        directionCount += used ? 1U : 0U;
    }
    for (const bool used : activeDirections)
    {
        activeDirectionCount += used ? 1U : 0U;
    }
    if (queuedBytes > transmitOpportunityQueuedBytes_ && ready > config_.transmitDMAStreams)
    {
        counters_.transmitSerializationDelayedBytes +=
            queuedBytes - transmitOpportunityQueuedBytes_;
    }
    transmitOpportunityReadyTransfers_ = ready;
    transmitOpportunityDirections_ = directionCount;
    transmitOpportunityActiveDirections_ = activeDirectionCount;
    transmitOpportunityActiveLanes_ = activeLanes;
    transmitOpportunityQueuedTransfers_ = queued;
    transmitOpportunityQueuedBytes_ = queuedBytes;
    transmitOpportunityFIFOEmptyLanes_ = fifoEmptyLanes;
    transmitOpportunityFIFOFullLanes_ = fifoFullLanes;
    counters_.transmitMaximumQueueOccupancyObserved =
        std::max(counters_.transmitMaximumQueueOccupancyObserved, queued);
    transmitOpportunityLastTick_ = tick;
}

std::uint32_t TxController::firstHopDirection(std::uint32_t destination) const noexcept
{
    if (config_.meshWidth == 0 || config_.meshHeight == 0 || destination == config_.tileId)
    {
        return 0;
    }
    const std::uint32_t sourceX = config_.tileId % config_.meshWidth;
    const std::uint32_t sourceY = config_.tileId / config_.meshWidth;
    const std::uint32_t destinationX = destination % config_.meshWidth;
    const std::uint32_t destinationY = destination / config_.meshWidth;
    if (destinationX > sourceX)
    {
        return 1; // East
    }
    if (destinationX < sourceX)
    {
        return 2; // West
    }
    if (destinationY > sourceY)
    {
        return 3; // South
    }
    return 4; // North
}

void TxController::beginTransmitBlock(const PacketEvent::Metadata& metadata,
                                      std::uint32_t destination, const char* kind, bool burst,
                                      std::uint32_t words, std::uint32_t queueOccupancy)
{
    if (!activeTransmitBlock_.has_value())
    {
        activeTransmitBlock_ = TransmitBlock{
            nextTransmitBlockSequence_++,
            host_.now().value,
            metadata.routeId,
            metadata.executionId,
            destination,
            kind,
            burst,
            words,
            1,
            queueOccupancy,
        };
        return;
    }

    TransmitBlock& block = *activeTransmitBlock_;
    if (block.routeId != metadata.routeId || block.executionId != metadata.executionId ||
        block.destination != destination || block.burst != burst ||
        (!burst && block.words != words) || block.kind != kind)
    {
        resources_.output.fatal(
            CALL_INFO, -1,
            "tile %u changed the pending transmit while blocked "
            "(blocked route=%u execution=%llu destination=%u kind=%s "
            "burst=%u words=%u; retry route=%u execution=%llu "
            "destination=%u kind=%s burst=%u words=%u)\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(block.routeId),
            static_cast<unsigned long long>(block.executionId),
            static_cast<unsigned>(block.destination), block.kind.c_str(),
            static_cast<unsigned>(block.burst), static_cast<unsigned>(block.words),
            static_cast<unsigned>(metadata.routeId),
            static_cast<unsigned long long>(metadata.executionId),
            static_cast<unsigned>(destination), kind, static_cast<unsigned>(burst),
            static_cast<unsigned>(words));
    }
    ++block.retryCount;
    block.words = std::max(block.words, words);
    block.maximumQueueOccupancy = std::max(block.maximumQueueOccupancy, queueOccupancy);
}

void TxController::completeTransmitBlock()
{
    if (!activeTransmitBlock_.has_value())
    {
        return;
    }
    const std::uint64_t finishTick = host_.now().value;
    const TransmitBlock block = std::move(*activeTransmitBlock_);
    activeTransmitBlock_.reset();
    const std::uint64_t duration = finishTick >= block.startTick ? finishTick - block.startTick : 0;
    counters_.transmitBlockedTicks += duration;
    ++counters_.transmitBlockedEvents;
    counters_.transmitBlockedRetries += block.retryCount;
    counters_.transmitMaximumQueueOccupancy =
        std::max(counters_.transmitMaximumQueueOccupancy,
                 static_cast<std::uint64_t>(block.maximumQueueOccupancy));
    resources_.profile.recordTransmitBlocked(block.eventSequence, block.routeId, block.executionId,
                                             block.destination, block.kind.c_str(), block.words,
                                             block.startTick, finishTick, block.retryCount,
                                             block.maximumQueueOccupancy);
}

bool TxController::idle() const noexcept
{
    const bool streamsIdle = std::all_of(
        transmitDMAStreams_.begin(), transmitDMAStreams_.end(), [](const TransmitDMAStream& stream)
        { return !stream.dma.burst.has_value() && stream.dma.wordsInFlight == 0; });
    return !pendingTransmit_.has_value() && !singleTransmitDMA_.burst.has_value() && streamsIdle;
}

PacketEvent::Metadata TxController::describePacket(const std::vector<std::uint32_t>& payload,
                                                   std::uint32_t destination,
                                                   std::uint64_t readyTick,
                                                   std::uint64_t injectionTick,
                                                   OutgoingFrame& nextFrame) const
{
    PacketEvent::Metadata metadata;
    metadata.packetId = nextNetworkPacketId_;
    metadata.readyTick = readyTick;
    metadata.injectionTick = injectionTick;

    if (!nextFrame.active && payload.size() == config_.deploymentFrameHeaderWords &&
        payload[0] == config_.deploymentFrameMagic)
    {
        nextFrame.active = true;
        nextFrame.destination = destination;
        nextFrame.routeId = payload[1];
        nextFrame.executionId = static_cast<std::uint64_t>(payload[2]) |
                                (static_cast<std::uint64_t>(payload[3]) << 32U);
        nextFrame.logicalIteration = static_cast<std::uint64_t>(payload[4]) |
                                     (static_cast<std::uint64_t>(payload[5]) << 32U);
        nextFrame.remainingPayloadWords = payload[6];
        metadata.routeId = nextFrame.routeId;
        metadata.executionId = nextFrame.executionId;
        metadata.logicalIteration = nextFrame.logicalIteration;
        metadata.protocolWords = static_cast<std::uint32_t>(payload.size());
        return metadata;
    }

    if (nextFrame.active && nextFrame.destination == destination &&
        payload.size() <= nextFrame.remainingPayloadWords)
    {
        metadata.routeId = nextFrame.routeId;
        metadata.executionId = nextFrame.executionId;
        metadata.logicalIteration = nextFrame.logicalIteration;
        metadata.payloadWords = static_cast<std::uint32_t>(payload.size());
        nextFrame.remainingPayloadWords -= metadata.payloadWords;
        if (nextFrame.remainingPayloadWords == 0)
        {
            nextFrame = OutgoingFrame{};
        }
        return metadata;
    }

    metadata.payloadWords = static_cast<std::uint32_t>(payload.size());
    return metadata;
}

bool TxController::readyForGuest(bool burst) const noexcept
{
    return burst ? resources_.bridge.transmitBurstHasSpace() : resources_.bridge.transmitHasSpace();
}

TxController::Status TxController::status() const noexcept
{
    return Status{(pendingTransmit_.has_value() ? 1U : 0U) +
                  (singleTransmitDMA_.burst.has_value() ? 1U : 0U)};
}

} // namespace SST::Mittens
