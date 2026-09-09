#include "sst_config.h"
#include "rxController.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include <mittens/MemoryMap.h>

namespace SST::Mittens
{
RxController::RxController(Configuration config, Resources resources, Host host)
    : config_(config), resources_(resources), host_(std::move(host))
{
    receiveLaneAvailable_.resize(config.receiveDMAStreams, 0);
    for (std::uint32_t lane = 0; lane < config.receiveDMAStreams; ++lane)
        receiveDMAEngines_.emplace_back(config.receiveDMAWidthBits, config.receiveDMASetupCycles);
}

AddressRegion RxController::scratchpadRegion() const noexcept
{
    return {MITTENS_SCRATCHPAD_BASE, config_.scratchpadBytes};
}

void RxController::service()
{
    if (stopped_ || !resources_.bridge.open() || resources_.network == nullptr)
    {
        return;
    }
    refreshReceiveDMATransfers();
    serviceIncomingPackets();
    scheduleReceiveDMABursts();
}

void RxController::onNetworkCompletion()
{
    if (stopped_ || !resources_.bridge.open() || resources_.network == nullptr)
    {
        return;
    }
    completeReadyNetworkReceives();
    service();
}

bool RxController::onDMACompletion()
{
    if (stopped_ || !resources_.bridge.open())
    {
        return false;
    }
    refreshReceiveDMATransfers();

    auto transfer = receiveDMATransfersInFlight_.end();
    for (auto candidate = receiveDMATransfersInFlight_.begin();
         candidate != receiveDMATransfersInFlight_.end(); ++candidate)
    {
        if (!candidate->completionObserved)
        {
            transfer = candidate;
            break;
        }
    }
    if (transfer == receiveDMATransfersInFlight_.end())
    {
        resources_.output.fatal(CALL_INFO, -1,
                                "tile %u received an RX DMA timer without a transfer\n",
                                static_cast<unsigned>(config_.tileId));
    }

    const std::uint64_t currentCycle = cpuDomain().floor(host_.now()).value;
    if (currentCycle < transfer->completionCycle)
    {
        resources_.output.fatal(
            CALL_INFO, -1, "tile %u observed RX DMA burst %u too early at cycle %llu\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(transfer->burstIndex),
            static_cast<unsigned long long>(currentCycle));
    }

    transfer->completionObserved = true;

    const bool scratchpadDestination =
        config_.scratchpadEnabled && scratchpadRegion().contains(transfer->destination);
    if (!scratchpadDestination && resources_.memory != nullptr && config_.memoryTileStride != 0)
    {
        const std::uint64_t lineSize = config_.memoryCacheLineSize;
        const std::uint64_t byteCount =
            static_cast<std::uint64_t>(transfer->wordCount) * sizeof(std::uint32_t);
        const std::uint64_t firstGuest = transfer->destination - (transfer->destination % lineSize);
        const std::uint64_t lastGuest = (transfer->destination + byteCount - 1) -
                                        ((transfer->destination + byteCount - 1) % lineSize);
        for (std::uint64_t guest = firstGuest;; guest += lineSize)
        {
            const std::uint64_t timing =
                static_cast<std::uint64_t>(config_.tileId) * config_.memoryTileStride +
                (guest - config_.memoryGuestBase);
            auto* const request =
                new SST::Interfaces::StandardMem::FlushAddr(timing, lineSize, true, 1);
            receiveDMAInvalidations_.emplace(request->getID(), transfer->burstIndex);
            ++transfer->invalidationLines;
            ++transfer->invalidationResponsesPending;
            resources_.memory->send(request);
            if (guest == lastGuest)
            {
                break;
            }
        }
        return false;
    }

    authorizeReceiveDMA(*transfer);
    return true;
}

RxController::InvalidationResult
RxController::handleInvalidationResponse(SST::Interfaces::StandardMem::Request* request)
{
    if (request != nullptr)
    {
        const auto invalidation = receiveDMAInvalidations_.find(request->getID());
        if (invalidation != receiveDMAInvalidations_.end())
        {
            const std::uint32_t burstIndex = invalidation->second;
            receiveDMAInvalidations_.erase(invalidation);
            delete request;
            if (stopped_)
            {
                return InvalidationResult::Pending;
            }
            for (ReceiveDMATransfer& transfer : receiveDMATransfersInFlight_)
            {
                if (transfer.burstIndex != burstIndex)
                {
                    continue;
                }
                if (transfer.invalidationResponsesPending == 0)
                {
                    resources_.output.fatal(CALL_INFO, -1,
                                            "tile %u received an extra DMA invalidation "
                                            "response for burst %u\n",
                                            static_cast<unsigned>(config_.tileId),
                                            static_cast<unsigned>(burstIndex));
                }
                --transfer.invalidationResponsesPending;
                if (transfer.invalidationResponsesPending == 0)
                {
                    authorizeReceiveDMA(transfer);
                    return InvalidationResult::Authorized;
                }
                return InvalidationResult::Pending;
            }
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u lost DMA burst %u before invalidation "
                                    "completed\n",
                                    static_cast<unsigned>(config_.tileId),
                                    static_cast<unsigned>(burstIndex));
        }
    }

    return InvalidationResult::Unhandled;
}

void RxController::authorizeReceiveDMA(ReceiveDMATransfer& transfer)
{
    if (!resources_.bridge.authorizeReceiveDMA())
    {
        resources_.output.fatal(CALL_INFO, -1, "tile %u could not authorize RX DMA burst %u\n",
                                static_cast<unsigned>(config_.tileId),
                                static_cast<unsigned>(transfer.burstIndex));
    }

    transfer.authorized = true;
    ++counters_.receiveDMATransfers;
    counters_.receiveDMAWords += transfer.wordCount;
    counters_.receiveDMAActiveCycles += transfer.serviceCycles;
    resources_.profile.recordReceiveDMA(
        "complete", transfer.source, transfer.routeId, transfer.executionId,
        transfer.logicalIteration, transfer.burstIndex, transfer.wordCount, transfer.scheduleTick,
        transfer.startCycle, transfer.completionCycle, host_.now().value, transfer.serviceCycles,
        transfer.invalidationLines);

    resources_.output.verbose(
        CALL_INFO, 2, 0,
        "tile %u completed RX DMA burst %u from tile %u "
        "(route=%u, words=%u, dma_cycle=%llu)\n",
        static_cast<unsigned>(config_.tileId), static_cast<unsigned>(transfer.burstIndex),
        static_cast<unsigned>(transfer.source), static_cast<unsigned>(transfer.routeId),
        static_cast<unsigned>(transfer.wordCount),
        static_cast<unsigned long long>(receiveDMADomain().floor(host_.now()).value));
}

void RxController::serviceIncomingPackets()
{
    constexpr int kVirtualNetwork = 0;
    const std::size_t completedFrameCapacity =
        static_cast<std::size_t>(std::max(config_.receiveDMAQueueDepth, 1U)) *
        static_cast<std::size_t>(std::max(config_.networkSize, 1U));

    flushReadyReceiveBursts();
    // Keep one completed network packet staged outside the four-entry bridge
    // while an earlier descriptor-backed payload waits for RX-DMA service.
    // The bridge remains physically bounded by receiveDMAQueueDepth; this
    // extra admission slot prevents network backpressure from hiding the
    // next cross-source header behind that bridge head.
    const std::uint32_t receiveAdmissionCapacity = config_.receiveDMAQueueDepth + 1U;
    while (
        // An in-progress frame is not part of the completed-frame
        // capacity, but its remaining fragments must still be admitted;
        // otherwise a full completed queue can strand that frame forever.
        (completedReceiveFrameCount_ < completedFrameCapacity ||
         !incomingFrameAssemblies_.empty()) &&
        resources_.bridge.receiveBurstCount() + pendingNetworkReceives_.size() <
            receiveAdmissionCapacity &&
        resources_.network->requestToReceive(kVirtualNetwork))
    {
        SST::Interfaces::SimpleNetwork::Request* request =
            resources_.network->recv(kVirtualNetwork);
        if (request == nullptr)
        {
            return;
        }

        SST::Event* rawPayload = request->takePayload();
        auto* packet = dynamic_cast<PacketEvent*>(rawPayload);
        if (packet == nullptr)
        {
            delete rawPayload;
            delete request;
            resources_.output.fatal(CALL_INFO, -1, "tile %u received an invalid network payload\n",
                                    static_cast<unsigned>(config_.tileId));
        }

        const auto source = request->src;
        const PacketEvent::Metadata metadata = packet->metadata();
        resources_.profile.recordReceiveBoundary("nic-dequeue", static_cast<std::uint32_t>(source),
            metadata.routeId, metadata.executionId, metadata.logicalIteration,
            static_cast<std::uint32_t>(packet->payloads().size()), host_.now().value);
        std::vector<std::uint32_t> payload = packet->payloads();
        delete packet;
        delete request;

        if (payload.empty() || payload.size() > MITTENS_BRIDGE_BURST_WORD_CAPACITY)
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u received an invalid %zu-word "
                                    "network burst\n",
                                    static_cast<unsigned>(config_.tileId), payload.size());
        }
        const std::uint64_t headArrivalTick = host_.now().value;
        if (payload.size() > std::numeric_limits<std::uint64_t>::max() / 32)
        {
            resources_.output.fatal(CALL_INFO, -1, "tile %u network burst size overflowed\n",
                                    static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t transferCycles =
            config_.networkTailDelivery
                ? 0
                : Timing::ceilDivide(static_cast<std::uint64_t>(payload.size()) * 32,
                                     config_.meshLinkWidthBits);
        const std::uint64_t linkPeriod = networkDomain().factor();
        if (transferCycles > std::numeric_limits<std::uint64_t>::max() / linkPeriod)
        {
            resources_.output.fatal(CALL_INFO, -1, "tile %u network completion time overflowed\n",
                                    static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t transferTicks = networkDomain().ticks({transferCycles}).value;
        const std::uint64_t startTick =
            config_.networkTailDelivery
                ? headArrivalTick
                : std::max(headArrivalTick, networkReceiveNextAvailableTick_);
        if (startTick > std::numeric_limits<std::uint64_t>::max() - transferTicks)
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u network receive serialization overflowed\n",
                                    static_cast<unsigned>(config_.tileId));
        }
        networkReceiveNextAvailableTick_ = startTick + transferTicks;
        const std::uint64_t completionTick = config_.networkTailDelivery
                                                 ? headArrivalTick
                                                 : networkReceiveNextAvailableTick_ - linkPeriod;
        const std::uint64_t completionDelayCycles =
            Timing::ceilDivide(completionTick - headArrivalTick, linkPeriod);
        pendingNetworkReceives_.push_back(PendingNetworkReceive{
            static_cast<std::uint32_t>(source),
            metadata,
            std::move(payload),
            completionTick,
        });
        if (completionDelayCycles == 0)
        {
            completeReadyNetworkReceives();
        }
        else
        {
            host_.scheduleNetworkCompletion({completionDelayCycles});
        }
    }
}

void RxController::completeReadyNetworkReceives()
{
    const std::uint64_t now = host_.now().value;
    auto receive = pendingNetworkReceives_.begin();
    while (receive != pendingNetworkReceives_.end())
    {
        if (receive->completionTick > now)
        {
            ++receive;
            continue;
        }
        PendingNetworkReceive completed = std::move(*receive);
        receive = pendingNetworkReceives_.erase(receive);
        completeNetworkReceive(std::move(completed));
    }
}

void RxController::completeNetworkReceive(PendingNetworkReceive receive)
{
    const std::size_t words = receive.payload.size();
    const bool frameHeader = receive.metadata.protocolWords != 0 &&
                             receive.payload.size() == config_.deploymentFrameHeaderWords &&
                             receive.payload[0] == config_.deploymentFrameMagic;
    const bool framePayload = receive.metadata.routeId != PacketEvent::InvalidRouteId &&
                              receive.metadata.payloadWords != 0;

    if (frameHeader)
    {
        if (receive.payload[6] == 0 ||
            incomingFrameAssemblies_.find(receive.source) != incomingFrameAssemblies_.end())
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u received an invalid or overlapping frame "
                                    "header from tile %u\n",
                                    static_cast<unsigned>(config_.tileId),
                                    static_cast<unsigned>(receive.source));
        }
        IncomingFrameAssembly assembly{
            receive.metadata.routeId,
            receive.metadata.executionId,
            receive.metadata.logicalIteration,
            receive.payload[6],
            std::move(receive.payload),
            {},
            false,
            false,
            0,
            false,
            ReceivePayloadOwner::Unclaimed,
        };
        assembly.payload.reserve(assembly.expectedWords);
        incomingFrameAssemblies_.emplace(receive.source, std::move(assembly));
        exposeNextReceiveHeader(receive.source);
    }
    else if (framePayload)
    {
        const auto found = incomingFrameAssemblies_.find(receive.source);
        if (found == incomingFrameAssemblies_.end() ||
            found->second.routeId != receive.metadata.routeId ||
            found->second.executionId != receive.metadata.executionId ||
            found->second.logicalIteration != receive.metadata.logicalIteration ||
            receive.payload.size() > found->second.expectedWords - found->second.payload.size())
        {
            resources_.output.fatal(
                CALL_INFO, -1,
                "tile %u received an invalid frame payload from "
                "tile %u (route=%u, words=%zu)\n",
                static_cast<unsigned>(config_.tileId), static_cast<unsigned>(receive.source),
                static_cast<unsigned>(receive.metadata.routeId), receive.payload.size());
        }
        found->second.payload.insert(found->second.payload.end(), receive.payload.begin(),
                                     receive.payload.end());
        if (found->second.payload.size() == found->second.expectedWords)
        {
            resources_.profile.recordReceiveBoundary("frame-ready", receive.source,
                receive.metadata.routeId, receive.metadata.executionId,
                receive.metadata.logicalIteration, found->second.expectedWords, host_.now().value);
            IncomingFrameAssembly completed = std::move(found->second);
            incomingFrameAssemblies_.erase(found);
            completedReceiveFrames_[receive.source].push_back(std::move(completed));
            ++completedReceiveFrameCount_;
            releaseCompletedReceiveFrames(receive.source);
        }
        else if (config_.receiveDMAStreaming)
        {
            releaseCompletedReceiveFrames(receive.source);
        }
    }
    else
    {
        readyReceiveBursts_.push_back(ReadyReceiveBurst{
            receive.source,
            std::move(receive.payload),
            true,
        });
    }
    flushReadyReceiveBursts();

    ++counters_.networkReceivePackets;
    counters_.networkReceiveWords += words;
    const std::uint64_t arrivalTick = host_.now().value;
    if (arrivalTick >= receive.metadata.injectionTick)
    {
        counters_.networkTransitTicks += arrivalTick - receive.metadata.injectionTick;
    }
    if (receive.metadata.routeId != PacketEvent::InvalidRouteId &&
        receive.metadata.protocolWords != 0)
    {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(receive.source) << 32U) | receive.metadata.routeId;
        incomingRouteExecutions_[key].push_back(IncomingRouteTag{
            receive.metadata.executionId,
            receive.metadata.logicalIteration,
        });
    }
    resources_.profile.recordNetwork(
        "arrive", receive.metadata.packetId, receive.source, config_.tileId,
        receive.metadata.routeId, receive.metadata.executionId, receive.metadata.logicalIteration,
        receive.metadata.protocolWords != 0
            ? "frame-header"
            : (receive.metadata.payloadWords != 0 ? "frame-payload" : "raw"),
        static_cast<std::uint32_t>(words), receive.metadata.protocolWords,
        receive.metadata.payloadWords, host_.meshHops(receive.source, config_.tileId),
        receive.metadata.readyTick, receive.metadata.injectionTick, arrivalTick);

    resources_.output.verbose(
        CALL_INFO, 2, 0, "tile %u completed %zu-word network burst from tile %u\n",
        static_cast<unsigned>(config_.tileId), words, static_cast<unsigned>(receive.source));
}

void RxController::exposeNextReceiveHeader(std::uint32_t source)
{
    const std::size_t exposureLimit =
        static_cast<std::size_t>(std::max(config_.receiveDMAQueueDepth, 1U));
    std::size_t exposed = 0;
    auto completed = completedReceiveFrames_.find(source);
    if (completed != completedReceiveFrames_.end() && !completed->second.empty())
    {
        for (const IncomingFrameAssembly& frame : completed->second)
        {
            exposed += frame.headerExposed ? 1U : 0U;
        }
    }
    auto incoming = incomingFrameAssemblies_.find(source);
    if (incoming != incomingFrameAssemblies_.end())
    {
        exposed += incoming->second.headerExposed ? 1U : 0U;
    }

    auto expose = [&](IncomingFrameAssembly& frame)
    {
        if (frame.headerExposed || exposed >= exposureLimit)
        {
            return;
        }
        readyReceiveBursts_.push_back(ReadyReceiveBurst{
            source,
            std::move(frame.header),
            true,
            frame.routeId,
            frame.executionId,
            frame.logicalIteration,
            false,
        });
        frame.headerExposed = true;
        resources_.profile.recordReceiveBoundary("header-exposed", source, frame.routeId,
            frame.executionId, frame.logicalIteration, frame.expectedWords, host_.now().value);
        ++exposed;
    };
    bool precedingPayloadQueued = true;
    if (completed != completedReceiveFrames_.end())
    {
        for (IncomingFrameAssembly& frame : completed->second)
        {
            if (!precedingPayloadQueued)
            {
                break;
            }
            expose(frame);
            precedingPayloadQueued = frame.payloadQueued;
        }
    }
    if (precedingPayloadQueued && incoming != incomingFrameAssemblies_.end())
    {
        expose(incoming->second);
    }
    flushReadyReceiveBursts();
}

void RxController::releaseCompletedReceiveFrames(std::uint32_t source)
{
    // Only explicitly DMA-owned frames may expose early, device-private data.
    // Header ownership and the existing bridge admission limit remain intact.
    auto incoming = incomingFrameAssemblies_.find(source);
    if (config_.receiveDMAStreaming && incoming != incomingFrameAssemblies_.end())
    {
        auto& frame = incoming->second;
        if (frame.headerExposed && frame.payloadOwner == ReceivePayloadOwner::DMA)
        {
            while (frame.queuedWords < frame.payload.size())
            {
                const auto count = std::min(frame.payload.size() - frame.queuedWords,
                    static_cast<std::size_t>(MITTENS_BRIDGE_BURST_WORD_CAPACITY));
                const auto begin = frame.payload.begin() + frame.queuedWords;
                readyReceiveBursts_.push_back(ReadyReceiveBurst{
                    source, std::vector<std::uint32_t>(begin, begin + count), false,
                    frame.routeId, frame.executionId, frame.logicalIteration, false});
                frame.queuedWords += count;
            }
            flushReadyReceiveBursts();
        }
    }
    auto frames = completedReceiveFrames_.find(source);
    if (frames == completedReceiveFrames_.end() || frames->second.empty())
    {
        return;
    }
    for (IncomingFrameAssembly& frame : frames->second)
    {
        if (frame.payloadQueued)
        {
            continue;
        }
        if (!frame.headerExposed)
        {
            break;
        }
        if (frame.payloadOwner == ReceivePayloadOwner::Unclaimed)
        {
            break;
        }
        if (frame.payloadOwner == ReceivePayloadOwner::DMA)
        {
            const auto descriptors = receiveDMADescriptors_.find(source);
            const bool descriptorMatches =
                descriptors != receiveDMADescriptors_.end() &&
                std::any_of(descriptors->second.begin(), descriptors->second.end(),
                            [&](const ReceiveDMADescriptor& descriptor)
                            {
                                return descriptor.routeId == frame.routeId &&
                                       descriptor.executionId == frame.executionId &&
                                       descriptor.logicalIteration == frame.logicalIteration &&
                                       (config_.receiveDMAStreaming ||
                                        descriptor.remainingWords == frame.expectedWords);
                            });
            if (!descriptorMatches)
            {
                resources_.output.fatal(CALL_INFO, -1,
                                        "tile %u lost the RX DMA owner for route %u "
                                        "iteration %llu from tile %u\n",
                                        static_cast<unsigned>(config_.tileId),
                                        static_cast<unsigned>(frame.routeId),
                                        static_cast<unsigned long long>(frame.logicalIteration),
                                        static_cast<unsigned>(source));
            }
        }
        auto insertion = std::find_if(readyReceiveBursts_.begin(), readyReceiveBursts_.end(),
                                      [&](const ReadyReceiveBurst& receive)
                                      {
                                          return receive.softwareVisible &&
                                                 receive.source == source &&
                                                 receive.routeId == frame.routeId &&
                                                 receive.executionId == frame.executionId &&
                                                 receive.logicalIteration == frame.logicalIteration;
                                      });
        if (insertion != readyReceiveBursts_.end())
        {
            ++insertion;
        }
        else
        {
            insertion = std::find_if(readyReceiveBursts_.begin(), readyReceiveBursts_.end(),
                                     [](const ReadyReceiveBurst& receive)
                                     { return receive.softwareVisible; });
        }
        for (std::size_t offset = frame.queuedWords; offset < frame.payload.size();
             offset += MITTENS_BRIDGE_BURST_WORD_CAPACITY)
        {
            const std::size_t count =
                std::min(frame.payload.size() - offset,
                         static_cast<std::size_t>(MITTENS_BRIDGE_BURST_WORD_CAPACITY));
            /*
             * The guest explicitly selected this frame's payload owner after
             * consuming its header. Queue software-owned payload as visible
             * words and DMA-owned payload as device-private bursts. Later
             * headers from this source remain behind the full payload.
             */
            insertion = readyReceiveBursts_.insert(
                insertion, ReadyReceiveBurst{
                               source,
                               std::vector<std::uint32_t>(frame.payload.begin() + offset,
                                                          frame.payload.begin() + offset + count),
                               frame.payloadOwner == ReceivePayloadOwner::Software,
                               frame.routeId,
                               frame.executionId,
                               frame.logicalIteration,
                               offset + count == frame.payload.size(),
                           });
            ++insertion;
        }
        frame.payloadQueued = true;
    }
    exposeNextReceiveHeader(source);
    flushReadyReceiveBursts();
}

void RxController::retireConsumedReceiveFrames(std::uint32_t source)
{
    auto frames = completedReceiveFrames_.find(source);
    if (frames == completedReceiveFrames_.end() || frames->second.empty())
    {
        return;
    }
    const std::uint32_t readIndex = resources_.bridge.receiveBurstReadIndex();
    auto frame = frames->second.begin();
    while (frame != frames->second.end())
    {
        if (!frame->payloadQueued || !frame->finalPayloadBurstPublished)
        {
            break;
        }
        const std::int32_t consumedDistance =
            static_cast<std::int32_t>(readIndex - frame->finalPayloadBurstIndex);
        if (consumedDistance <= 0)
        {
            break;
        }
        auto descriptors = receiveDMADescriptors_.find(source);
        const bool descriptorPending =
            descriptors != receiveDMADescriptors_.end() &&
            std::any_of(descriptors->second.begin(), descriptors->second.end(),
                        [&](const ReceiveDMADescriptor& descriptor)
                        {
                            return descriptor.routeId == frame->routeId &&
                                   descriptor.executionId == frame->executionId &&
                                   descriptor.logicalIteration == frame->logicalIteration;
                        });
        const bool transferPending =
            std::any_of(receiveDMATransfersInFlight_.begin(), receiveDMATransfersInFlight_.end(),
                        [&](const ReceiveDMATransfer& transfer)
                        {
                            return transfer.source == source &&
                                   transfer.routeId == frame->routeId &&
                                   transfer.executionId == frame->executionId &&
                                   transfer.logicalIteration == frame->logicalIteration;
                        });
        if (descriptorPending || transferPending)
        {
            break;
        }
        frame = frames->second.erase(frame);
        --completedReceiveFrameCount_;
    }
    if (frames->second.empty())
    {
        completedReceiveFrames_.erase(frames);
    }
    exposeNextReceiveHeader(source);
    releaseCompletedReceiveFrames(source);
}

void RxController::flushReadyReceiveBursts()
{
    while (!readyReceiveBursts_.empty() && resources_.bridge.receiveHasBurstSpace())
    {
        auto ready = readyReceiveBursts_.begin();
        /*
         * Framed traffic is admitted as an indivisible header-then-payload
         * sequence.  Do not bypass a queued header to fill a reserved bridge
         * slot: payload release no longer implies that its header was already
         * consumed, and such a bypass would expose payload as a new frame.
         */
        ReadyReceiveBurst& receive = *ready;
        const std::uint32_t burstIndex = resources_.bridge.receiveBurstWriteIndex();
        if (!resources_.bridge.pushReceiveBurst(receive.source, receive.payload,
                                                receive.softwareVisible))
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u bridge rejected a reassembled %zu-word "
                                    "receive burst from tile %u\n",
                                    static_cast<unsigned>(config_.tileId), receive.payload.size(),
                                    static_cast<unsigned>(receive.source));
        }
        if (receive.finalFramePayload)
        {
            auto frames = completedReceiveFrames_.find(receive.source);
            if (frames == completedReceiveFrames_.end())
            {
                resources_.output.fatal(CALL_INFO, -1,
                                        "tile %u lost receive-frame ownership for route %u "
                                        "iteration %llu from tile %u\n",
                                        static_cast<unsigned>(config_.tileId),
                                        static_cast<unsigned>(receive.routeId),
                                        static_cast<unsigned long long>(receive.logicalIteration),
                                        static_cast<unsigned>(receive.source));
            }
            const auto frame =
                std::find_if(frames->second.begin(), frames->second.end(),
                             [&](const IncomingFrameAssembly& candidate)
                             {
                                 return candidate.routeId == receive.routeId &&
                                        candidate.executionId == receive.executionId &&
                                        candidate.logicalIteration == receive.logicalIteration;
                             });
            if (frame == frames->second.end() || frame->finalPayloadBurstPublished)
            {
                resources_.output.fatal(CALL_INFO, -1,
                                        "tile %u lost receive-frame ownership for route %u "
                                        "iteration %llu from tile %u\n",
                                        static_cast<unsigned>(config_.tileId),
                                        static_cast<unsigned>(receive.routeId),
                                        static_cast<unsigned long long>(receive.logicalIteration),
                                        static_cast<unsigned>(receive.source));
            }
            frame->finalPayloadBurstIndex = burstIndex;
            frame->finalPayloadBurstPublished = true;
        }
        readyReceiveBursts_.erase(ready);
    }
}

RxController::IncomingFrameAssembly*
RxController::findReceiveFrame(std::uint32_t source, std::uint32_t routeId,
                               std::uint64_t executionId, std::uint64_t logicalIteration) noexcept
{
    auto incoming = incomingFrameAssemblies_.find(source);
    if (incoming != incomingFrameAssemblies_.end())
    {
        IncomingFrameAssembly& frame = incoming->second;
        if (frame.routeId == routeId && frame.executionId == executionId &&
            frame.logicalIteration == logicalIteration)
        {
            return &frame;
        }
    }
    auto completed = completedReceiveFrames_.find(source);
    if (completed == completedReceiveFrames_.end())
    {
        return nullptr;
    }
    const auto frame = std::find_if(completed->second.begin(), completed->second.end(),
                                    [&](const IncomingFrameAssembly& candidate)
                                    {
                                        return candidate.routeId == routeId &&
                                               candidate.executionId == executionId &&
                                               candidate.logicalIteration == logicalIteration;
                                    });
    return frame == completed->second.end() ? nullptr : &*frame;
}

std::uint64_t RxController::claimReceiveFrame(const ReceiveClaim& event,
                                             ReceivePayloadOwner owner)
{
    const char* claim = owner == ReceivePayloadOwner::DMA ? "RX DMA descriptor"
                                                         : "software payload claim";
    std::uint64_t executionId = 0;
    if (!takeRouteExecutionId(event.source, event.routeId, event.logicalIteration, &executionId))
    {
        resources_.output.fatal(CALL_INFO, -1,
                                "tile %u %s did not match a frame tag "
                                "(source=%u, route=%u, iteration=%llu)\n",
                                static_cast<unsigned>(config_.tileId), claim,
                                static_cast<unsigned>(event.source),
                                static_cast<unsigned>(event.routeId),
                                static_cast<unsigned long long>(event.logicalIteration));
    }
    IncomingFrameAssembly* frame =
        findReceiveFrame(event.source, event.routeId, executionId, event.logicalIteration);
    if (frame == nullptr || frame->expectedWords != event.wordCount ||
        frame->payloadOwner != ReceivePayloadOwner::Unclaimed)
    {
        resources_.output.fatal(CALL_INFO, -1,
                                "tile %u %s did not claim exactly one "
                                "unowned frame (source=%u, route=%u, iteration=%llu, "
                                "words=%u)\n",
                                static_cast<unsigned>(config_.tileId), claim,
                                static_cast<unsigned>(event.source),
                                static_cast<unsigned>(event.routeId),
                                static_cast<unsigned long long>(event.logicalIteration),
                                static_cast<unsigned>(event.wordCount));
    }
    frame->payloadOwner = owner;
    return executionId;
}

void RxController::registerReceiveDMA(const ReceiveClaim& event)
{
    if (stopped_)
    {
        return;
    }
    const std::uint64_t byteCount =
        static_cast<std::uint64_t>(event.wordCount) * sizeof(std::uint32_t);
    const bool streaming = event.logicalIteration != UINT64_MAX;
    const bool startsInScratchpad =
        config_.scratchpadEnabled && scratchpadRegion().contains(event.destination);
    const bool fitsScratchpad =
        startsInScratchpad && scratchpadRegion().containsRange(event.destination, byteCount);
    if (event.source >= config_.networkSize || event.routeId == UINT32_MAX ||
        event.wordCount == 0 || !AddressRegion::representable(event.destination, byteCount) ||
        (streaming && !fitsScratchpad) || (startsInScratchpad && !fitsScratchpad) ||
        (config_.scratchpadEnabled &&
         scratchpadRegion().crossesStart(event.destination, byteCount)) ||
        (!fitsScratchpad && config_.memoryTileStride != 0 &&
         !AddressRegion{config_.memoryGuestBase, config_.memoryTileStride}.containsRange(
             event.destination, byteCount)))
    {
        resources_.output.fatal(
            CALL_INFO, -1,
            "tile %u received invalid RX DMA descriptor "
            "(source=%u, route=%u, words=%u)\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(event.source),
            static_cast<unsigned>(event.routeId), static_cast<unsigned>(event.wordCount));
    }

    const std::uint64_t executionId = claimReceiveFrame(event, ReceivePayloadOwner::DMA);
    resources_.profile.recordReceiveBoundary("descriptor", event.source, event.routeId,
        executionId, event.logicalIteration, event.wordCount, host_.now().value);
    receiveDMADescriptors_[event.source].push_back(ReceiveDMADescriptor{
        event.routeId,
        executionId,
        event.logicalIteration,
        event.destination,
        event.wordCount,
        false,
    });
    releaseCompletedReceiveFrames(event.source);

    resources_.output.verbose(
        CALL_INFO, 2, 0,
        "tile %u registered RX DMA from tile %u "
        "(route=%u, words=%u)\n",
        static_cast<unsigned>(config_.tileId), static_cast<unsigned>(event.source),
        static_cast<unsigned>(event.routeId), static_cast<unsigned>(event.wordCount));
}

void RxController::registerReceiveSoftwareClaim(const ReceiveClaim& event)
{
    if (stopped_)
    {
        return;
    }
    if (event.source >= config_.networkSize || event.routeId == UINT32_MAX || event.wordCount == 0)
    {
        resources_.output.fatal(
            CALL_INFO, -1,
            "tile %u received invalid software payload claim "
            "(source=%u, route=%u, words=%u)\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(event.source),
            static_cast<unsigned>(event.routeId), static_cast<unsigned>(event.wordCount));
    }

    claimReceiveFrame(event, ReceivePayloadOwner::Software);
    releaseCompletedReceiveFrames(event.source);

    resources_.output.verbose(
        CALL_INFO, 2, 0,
        "tile %u registered software payload from tile %u "
        "(route=%u, words=%u)\n",
        static_cast<unsigned>(config_.tileId), static_cast<unsigned>(event.source),
        static_cast<unsigned>(event.routeId), static_cast<unsigned>(event.wordCount));
}

bool RxController::receiveBurstScheduled(std::uint32_t burstIndex) const noexcept
{
    for (const ReceiveDMATransfer& transfer : receiveDMATransfersInFlight_)
    {
        if (transfer.burstIndex == burstIndex)
        {
            return true;
        }
    }
    return false;
}

void RxController::refreshReceiveDMATransfers()
{
    if (!resources_.bridge.open())
    {
        receiveDMATransfersInFlight_.clear();
        return;
    }

    const std::uint32_t readIndex = resources_.bridge.receiveBurstReadIndex();
    while (!receiveDMATransfersInFlight_.empty())
    {
        const std::uint32_t burstIndex = receiveDMATransfersInFlight_.front().burstIndex;
        const std::int32_t consumedDistance = static_cast<std::int32_t>(readIndex - burstIndex);
        if (consumedDistance <= 0)
        {
            break;
        }
        if (!receiveDMATransfersInFlight_.front().authorized)
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u consumed RX DMA burst %u before authorization "
                                    "(read index=%u)\n",
                                    static_cast<unsigned>(config_.tileId),
                                    static_cast<unsigned>(burstIndex),
                                    static_cast<unsigned>(readIndex));
        }
        receiveDMATransfersInFlight_.pop_front();
    }
    std::vector<std::uint32_t> completedSources;
    completedSources.reserve(completedReceiveFrames_.size());
    for (const auto& [source, frames] : completedReceiveFrames_)
    {
        (void)frames;
        completedSources.push_back(source);
    }
    for (std::uint32_t source : completedSources)
    {
        retireConsumedReceiveFrames(source);
    }
}

void RxController::scheduleReceiveDMABursts()
{
    if (stopped_ || !resources_.bridge.open() || !host_.scheduleDMACompletion)
    {
        return;
    }

    refreshReceiveDMATransfers();
    const std::uint32_t burstCount = resources_.bridge.receiveBurstCount();
    for (std::uint32_t offset = 0; offset < burstCount; ++offset)
    {
        const std::optional<ReceiveBurstInfo> burst = resources_.bridge.peekReceiveBurst(offset);
        if (!burst.has_value())
        {
            return;
        }
        if (receiveBurstScheduled(burst->absoluteIndex))
        {
            continue;
        }
        if (burst->softwareVisible)
        {
            continue;
        }

        auto descriptor = receiveDMADescriptors_.find(burst->source);
        if (descriptor == receiveDMADescriptors_.end() || descriptor->second.empty())
        {
            // A private payload cannot be scheduled without its owner.
            // Software-visible headers were skipped above.
            return;
        }
        ReceiveDMADescriptor& activeDescriptor = descriptor->second.front();
        if (burst->wordCount > activeDescriptor.remainingWords)
        {
            resources_.output.fatal(CALL_INFO, -1,
                                    "tile %u RX DMA burst from tile %u has %u words, "
                                    "but route %u expects only %u more\n",
                                    static_cast<unsigned>(config_.tileId),
                                    static_cast<unsigned>(burst->source),
                                    static_cast<unsigned>(burst->wordCount),
                                    static_cast<unsigned>(activeDescriptor.routeId),
                                    static_cast<unsigned>(activeDescriptor.remainingWords));
        }

        const std::uint64_t currentCycle = cpuDomain().floor(host_.now()).value;
        const auto lane = static_cast<std::size_t>(std::min_element(
            receiveLaneAvailable_.begin(), receiveLaneAvailable_.end()) - receiveLaneAvailable_.begin());
        const std::uint64_t earliestCycle = std::max({currentCycle,
            receiveLaneAvailable_[lane], receiveSourceAvailable_[burst->source]});
        ScratchpadSchedule timing{};
        try
        {
            const bool scratchpadDestination =
                config_.scratchpadEnabled &&
                scratchpadRegion().contains(activeDescriptor.destination);
            if (scratchpadDestination)
            {
                timing = resources_.scratchpad->scheduleDMA(
                    earliestCycle, scratchpadRegion().offset(activeDescriptor.destination),
                    static_cast<std::uint64_t>(burst->wordCount) * sizeof(std::uint32_t), true,
                    static_cast<ScratchpadDMAClient>(static_cast<unsigned>(ScratchpadDMAClient::NetworkReceive) + lane), !activeDescriptor.setupCharged);
            }
            else
            {
                // Ordinary-memory RX retains the configured DMA clock/rate.
                // Transfer records and the self-link use CPU cycles, so round
                // converted timestamps up: completion must never be early.
                const auto dma = receiveDMAEngines_[lane].schedule(
                    receiveDMADomain().ceil(cpuDomain().ticks({earliestCycle})).value,
                    burst->wordCount, !activeDescriptor.setupCharged);
                timing.startCycle =
                    cpuDomain().ceil(receiveDMADomain().ticks({dma.startCycle})).value;
                timing.completionCycle =
                    cpuDomain().ceil(receiveDMADomain().ticks({dma.completionCycle})).value;
                timing.serviceCycles = timing.completionCycle - timing.startCycle;
            }
        }
        catch (const std::exception& error)
        {
            resources_.output.fatal(CALL_INFO, -1, "tile %u cannot schedule RX DMA: %s\n",
                                    static_cast<unsigned>(config_.tileId), error.what());
        }

        receiveLaneAvailable_[lane] = timing.completionCycle;
        receiveSourceAvailable_[burst->source] = timing.completionCycle;
        const std::uint64_t destination = activeDescriptor.destination;
        activeDescriptor.destination +=
            static_cast<std::uint64_t>(burst->wordCount) * sizeof(std::uint32_t);
        activeDescriptor.setupCharged = true;
        activeDescriptor.remainingWords -= burst->wordCount;
        const std::uint32_t routeId = activeDescriptor.routeId;
        const std::uint64_t executionId = activeDescriptor.executionId;
        const std::uint64_t logicalIteration = activeDescriptor.logicalIteration;
        const std::uint64_t scheduleTick = host_.now().value;
        receiveDMATransfersInFlight_.push_back(ReceiveDMATransfer{
            burst->absoluteIndex,
            burst->source,
            routeId,
            executionId,
            logicalIteration,
            destination,
            burst->wordCount,
            scheduleTick,
            timing.startCycle,
            timing.completionCycle,
            timing.serviceCycles,
            0,
            0,
            false,
            false,
        });
        resources_.profile.recordReceiveDMA(
            "schedule", burst->source, routeId, executionId, logicalIteration, burst->absoluteIndex,
            burst->wordCount, scheduleTick, timing.startCycle, timing.completionCycle, scheduleTick,
            timing.serviceCycles);
        if (activeDescriptor.remainingWords == 0)
        {
            const std::uint32_t completedSource = burst->source;
            descriptor->second.pop_front();
            if (descriptor->second.empty())
            {
                receiveDMADescriptors_.erase(descriptor);
            }
            releaseCompletedReceiveFrames(completedSource);
        }

        host_.scheduleDMACompletion({timing.completionCycle - currentCycle});

        resources_.output.verbose(
            CALL_INFO, 3, 0,
            "tile %u scheduled RX DMA burst %u from tile %u "
            "(route=%u, words=%u, start=%llu, complete=%llu)\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(burst->absoluteIndex),
            static_cast<unsigned>(burst->source), static_cast<unsigned>(routeId),
            static_cast<unsigned>(burst->wordCount),
            static_cast<unsigned long long>(timing.startCycle),
            static_cast<unsigned long long>(timing.completionCycle));
    }
}

bool RxController::readyForGuest() const noexcept
{
    if (stopped_ || !resources_.bridge.open())
    {
        return false;
    }
    // RX-DMA completions live in QEMU's device-local queue. Publish their
    // presence through the shared bridge so SST cannot leave a guest asleep
    // after the final authorization has been consumed but before software
    // observes and acknowledges the completion.
    if (resources_.bridge.receiveDMACompletionAvailable())
    {
        return true;
    }
    // A timed RX-DMA burst is consumed from the bridge by QEMU only after
    // SST publishes its authorization.  At that point the burst may already
    // have left the bridge ring, so peeking the ring alone cannot wake a guest
    // that is sleeping in wait_for_receive().  The authorization is the
    // architectural handoff that requires QEMU to run and complete the DMA
    // (and subsequently acknowledge its completion).
    if (resources_.bridge.receiveDMAAuthorizationAvailable())
    {
        return true;
    }
    if (resources_.bridge.receiveHasWordData())
    {
        return true;
    }

    const std::optional<ReceiveBurstInfo> burst = resources_.bridge.peekReceiveBurst();
    if (!burst.has_value())
    {
        return false;
    }
    if (receiveBurstScheduled(burst->absoluteIndex))
    {
        return resources_.bridge.receiveDMAAuthorizationAvailable();
    }
    if (burst->softwareVisible)
    {
        const auto descriptors = receiveDMADescriptors_.find(burst->source);
        const bool descriptorActive =
            descriptors != receiveDMADescriptors_.end() && !descriptors->second.empty();
        const bool transferActive = std::any_of(
            receiveDMATransfersInFlight_.begin(), receiveDMATransfersInFlight_.end(),
            [&](const ReceiveDMATransfer& transfer) { return transfer.source == burst->source; });
        /*
         * QEMU deliberately hides a later header from a source while an
         * earlier frame from that source is waiting for RX-DMA timing.  Do
         * not wake a blocked guest merely because the bridge head is tagged
         * software-visible: without an authorization QEMU will reject that
         * same head and immediately yield again at the identical tick.
         */
        return (!descriptorActive && !transferActive) ||
               resources_.bridge.receiveDMAAuthorizationAvailable();
    }
    const auto descriptors = receiveDMADescriptors_.find(burst->source);
    if (descriptors != receiveDMADescriptors_.end() && !descriptors->second.empty())
    {
        return false;
    }
    // Unclaimed framed payloads are intentionally invisible. The guest must
    // explicitly select software delivery or register RX DMA after parsing
    // the header, so temporary ring-slot backpressure cannot be mistaken for
    // a software route.
    return false;
}

bool RxController::takeRouteExecutionId(std::uint32_t source, std::uint32_t routeId,
                                        std::uint64_t logicalIteration,
                                        std::uint64_t* executionId) noexcept
{
    const std::uint64_t key = (static_cast<std::uint64_t>(source) << 32U) | routeId;
    auto found = incomingRouteExecutions_.find(key);
    if (found == incomingRouteExecutions_.end() || found->second.empty() || executionId == nullptr)
    {
        return false;
    }
    auto tag = std::find_if(found->second.begin(), found->second.end(),
                            [logicalIteration](const IncomingRouteTag& candidate)
                            { return candidate.logicalIteration == logicalIteration; });
    if (tag == found->second.end())
    {
        return false;
    }
    *executionId = tag->executionId;
    found->second.erase(tag);
    if (found->second.empty())
    {
        incomingRouteExecutions_.erase(found);
    }
    return true;
}

RxController::Status RxController::status() const
{
    Status result;
    result.pendingNetwork = pendingNetworkReceives_.size() + readyReceiveBursts_.size() +
                            incomingFrameAssemblies_.size() + completedReceiveFrameCount_;
    result.pendingTransfers = receiveDMATransfersInFlight_.size();
    result.completedFrames = completedReceiveFrameCount_;
    result.readyBursts = readyReceiveBursts_.size();
    result.incomingFrames = incomingFrameAssemblies_.size();
    for (const auto& [source, descriptors] : receiveDMADescriptors_)
    {
        (void)source;
        result.pendingDescriptors += descriptors.size();
    }
    result.bridgeReceiveBursts =
        resources_.bridge.open() ? resources_.bridge.receiveBurstCount() : 0;
    result.bridgeHead =
        resources_.bridge.open() ? resources_.bridge.peekReceiveBurst() : std::nullopt;
    for (std::uint32_t offset = 0; offset < result.bridgeReceiveBursts; ++offset)
    {
        const std::optional<ReceiveBurstInfo> burst = resources_.bridge.peekReceiveBurst(offset);
        if (!burst.has_value() || burst->softwareVisible ||
            receiveBurstScheduled(burst->absoluteIndex))
        {
            continue;
        }
        result.firstUnscheduledPayloadOffset = offset;
        result.firstUnscheduledPayloadSource = burst->source;
        const auto descriptors = receiveDMADescriptors_.find(burst->source);
        if (descriptors != receiveDMADescriptors_.end())
        {
            result.firstUnscheduledPayloadDescriptorCount =
                static_cast<std::uint32_t>(descriptors->second.size());
            if (!descriptors->second.empty())
            {
                const ReceiveDMADescriptor& descriptor = descriptors->second.front();
                result.firstUnscheduledPayloadDescriptorRoute = descriptor.routeId;
                result.firstUnscheduledPayloadDescriptorIteration = descriptor.logicalIteration;
                result.firstUnscheduledPayloadDescriptorRemainingWords = descriptor.remainingWords;
            }
        }
        break;
    }
    result.authorizationAvailable = resources_.bridge.receiveDMAAuthorizationAvailable();
    result.bridgeHeadScheduled =
        result.bridgeHead.has_value() && receiveBurstScheduled(result.bridgeHead->absoluteIndex);
    return result;
}

} // namespace SST::Mittens
