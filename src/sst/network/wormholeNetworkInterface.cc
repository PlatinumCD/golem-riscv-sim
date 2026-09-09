#include "wormholeNetworkInterface.h"

#include "packetEvent.h"

#include <algorithm>
#include <limits>
#include <string>

namespace SST
{
namespace Mittens
{

WormholeNetworkInterface::WormholeNetworkInterface(SST::ComponentId_t id, SST::Params& params,
                                                   int virtualNetworks)
    : SST::Interfaces::SimpleNetwork(id),
      configuration_(NetworkInterfaceConfiguration::read(params)),
      endpointId_(configuration_.endpoint_id), networkSize_(configuration_.network_size),
      txStreams_(configuration_.tx_streams), rxStreams_(configuration_.rx_streams), flitsPerCycle_(configuration_.link_width_bits / 32),
      routerBufferFlits_(configuration_.router_buffer_flits),
      injectionBufferFlits_(configuration_.injection_buffer_flits),
      output_("mittens-wormhole-nic: ", configuration_.verbose, 0, SST::Output::STDOUT),
      linkBandwidth_("1b/s")
{
    configuration_.validate(output_, virtualNetworks);
    configuration_.emit();
    const auto tracePath = params.find<std::string>("receive_flit_trace_path", "");
    if (!tracePath.empty()) {
        flitTrace_.open(tracePath);
        if (!flitTrace_) output_.fatal(CALL_INFO, -1, "cannot open NIC receive trace %s\n", tracePath.c_str());
        flitTrace_ << "time_ps,source,packet_id,head,tail,lane\n";
    }
    const auto linkWidth = configuration_.link_width_bits;
    routerCredits_.fill(routerBufferFlits_);

    const std::string clock = configuration_.clock;
    clockTimeBase_ = getTimeConverter(clock);
    clockFactor_ = clockTimeBase_.getFactor();
    clockHandler_ =
        new SST::Clock::Handler<WormholeNetworkInterface, &WormholeNetworkInterface::clock>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
    SST::UnitAlgebra frequency(clock);
    linkBandwidth_ = frequency * SST::UnitAlgebra(std::to_string(linkWidth) + "b");
    for (std::uint32_t lane = 0; lane < std::max(txStreams_, rxStreams_); ++lane)
    {
        routerLinks_[lane] = configureLink(
            "router_port" + std::to_string(lane),
            new SST::Event::Handler<WormholeNetworkInterface,
                                    &WormholeNetworkInterface::handleRouterEvent, int>(
                this, static_cast<int>(lane)));
        if (routerLinks_[lane] == nullptr && lane == 0)
        {
            routerLinks_[lane] = configureLink(
                "router_port",
                new SST::Event::Handler<WormholeNetworkInterface,
                                        &WormholeNetworkInterface::handleRouterEvent, int>(this,
                                                                                           0));
        }
        if (routerLinks_[lane] == nullptr)
        {
            output_.fatal(CALL_INFO, -1, "endpoint %u requires local router lane %u\n",
                          static_cast<unsigned>(endpointId_), static_cast<unsigned>(lane));
        }
    }
    packetTailDeliveryLink_ = configureSelfLink(
        "packet-tail-delivery", clockTimeBase_,
        new SST::Event::Handler<WormholeNetworkInterface,
                                &WormholeNetworkInterface::handlePacketTailDelivery>(this));
    if (packetTailDeliveryLink_ == nullptr)
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u could not configure packet-tail delivery\n",
                      static_cast<unsigned>(endpointId_));
    }

    injectedFlits_ = registerStatistic<std::uint64_t>("injected_flits");
    receivedFlits_ = registerStatistic<std::uint64_t>("received_flits");
    completedPackets_ = registerStatistic<std::uint64_t>("completed_packets");
    injectionCreditStallCycles_ = registerStatistic<std::uint64_t>("injection_credit_stall_cycles");
    injectionQueueOccupancy_ = registerStatistic<std::uint64_t>("injection_queue_occupancy");
    packetNetworkLatency_ = registerStatistic<std::uint64_t>("packet_network_latency");
}

WormholeNetworkInterface::~WormholeNetworkInterface()
{
    delete receiveHandler_;
    delete sendHandler_;
    for (auto& queue : injectionQueues_)
    {
        while (!queue.empty())
        {
            delete queue.front();
            queue.pop_front();
        }
    }
    while (!receiveQueue_.empty())
    {
        delete receiveQueue_.front().request;
        receiveQueue_.pop_front();
    }
    while (!untimedReceiveQueue_.empty())
    {
        delete untimedReceiveQueue_.front();
        untimedReceiveQueue_.pop_front();
    }
    for (auto& [packet, request] : assembling_)
    {
        static_cast<void>(packet);
        delete request;
    }
}

void WormholeNetworkInterface::sendUntimedData(Request* request)
{
    if (request == nullptr)
    {
        return;
    }
    request->src = endpointId_;
    request->vn = 0;
    routerLinks_[0]->sendUntimedData(
        new WormholeFlitEvent(nextPacketId_++, endpointId_,
                              static_cast<std::uint32_t>(request->dest), 0, 0, 1, 0, request));
}

SST::Interfaces::SimpleNetwork::Request* WormholeNetworkInterface::recvUntimedData()
{
    if (untimedReceiveQueue_.empty())
    {
        return nullptr;
    }
    Request* request = untimedReceiveQueue_.front();
    untimedReceiveQueue_.pop_front();
    return request;
}

bool WormholeNetworkInterface::send(Request* request, int virtualNetwork)
{
    if (request == nullptr || virtualNetwork != 0 || !initialized_)
    {
        return false;
    }
    if (request->dest < 0 || request->dest >= static_cast<nid_t>(networkSize_))
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u received invalid destination %lld\n",
                      static_cast<unsigned>(endpointId_), static_cast<long long>(request->dest));
    }
    std::uint32_t lane = txStreams_;
    const auto* packet = dynamic_cast<const PacketEvent*>(request->inspectPayload());
    if (packet != nullptr && packet->metadata().injectionLane < txStreams_)
    {
        lane = packet->metadata().injectionLane;
    }
    if (lane == txStreams_)
    {
        // Untagged control traffic uses the least occupied available lane.
        // Do not hash by direction: lanes are independent injection engines,
        // not statically partitioned cardinal ports.
        for (std::uint32_t candidate = 0; candidate < txStreams_; ++candidate)
        {
            if (!injectionOrder_.accepts(static_cast<std::uint32_t>(request->dest), candidate))
                continue;
            if (lane == txStreams_ ||
                injectionQueues_[candidate].size() < injectionQueues_[lane].size())
            {
                lane = candidate;
            }
        }
    }
    if (!injectionOrder_.accepts(static_cast<std::uint32_t>(request->dest), lane))
    {
        // A finished DMA frame can still have flits in its NIC/router lane.
        // Do not let a new header on another lane overtake that frame's tail.
        // Other destinations and the currently owned lane remain independent.
        return false;
    }
    auto& queue = injectionQueues_[lane];
    const std::uint32_t count = flitCount(request->size_in_bits);
    if (queue.size() + count > injectionBufferFlits_)
    {
        return false;
    }

    request->src = endpointId_;
    request->vn = virtualNetwork;
    injectionOrder_.enqueue(static_cast<std::uint32_t>(request->dest), lane, count);
    const std::uint64_t packetId = nextPacketId_++;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        queue.push_back(new WormholeFlitEvent(packetId, endpointId_,
                                              static_cast<std::uint32_t>(request->dest),
                                              static_cast<std::uint32_t>(virtualNetwork), index,
                                              count, 0, index == 0 ? request : nullptr));
    }
    ensureClock();
    return true;
}

SST::Interfaces::SimpleNetwork::Request* WormholeNetworkInterface::recv(int virtualNetwork)
{
    if (virtualNetwork != 0 || receiveQueue_.empty())
    {
        return nullptr;
    }
    const ReceivedPacket packet = receiveQueue_.front();
    receiveQueue_.pop_front();
    if (packet.flits > ejectionOccupancy_[packet.lane])
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u has invalid ejection occupancy\n",
                      static_cast<unsigned>(endpointId_));
    }
    ejectionOccupancy_[packet.lane] -= packet.flits;
    routerLinks_[packet.lane]->send(new WormholeCreditEvent(packet.flits));
    return packet.request;
}

bool WormholeNetworkInterface::spaceToSend(int virtualNetwork, int bits)
{
    if (virtualNetwork != 0 || bits <= 0 || !initialized_)
    {
        return false;
    }
    const std::uint32_t count = flitCount(bits);
    return std::any_of(injectionQueues_.begin(), injectionQueues_.begin() + txStreams_,
                       [count, this](const auto& queue)
                       { return queue.size() + count <= injectionBufferFlits_; });
}

bool WormholeNetworkInterface::requestToReceive(int virtualNetwork)
{
    return virtualNetwork == 0 && !receiveQueue_.empty();
}

void WormholeNetworkInterface::setNotifyOnReceive(HandlerBase* handler)
{
    delete receiveHandler_;
    receiveHandler_ = handler;
}

void WormholeNetworkInterface::setNotifyOnSend(HandlerBase* handler)
{
    delete sendHandler_;
    sendHandler_ = handler;
}

bool WormholeNetworkInterface::isNetworkInitialized() const
{
    return initialized_;
}

SST::Interfaces::SimpleNetwork::nid_t WormholeNetworkInterface::getEndpointID() const
{
    return endpointId_;
}

const SST::UnitAlgebra& WormholeNetworkInterface::getLinkBW() const
{
    return linkBandwidth_;
}

void WormholeNetworkInterface::init(unsigned)
{
    initialized_ = true;
    while (SST::Event* raw = routerLinks_[0]->recvUntimedData())
    {
        auto* flit = dynamic_cast<WormholeFlitEvent*>(raw);
        if (flit == nullptr)
        {
            delete raw;
            output_.fatal(CALL_INFO, -1, "endpoint %u received invalid untimed data\n",
                          static_cast<unsigned>(endpointId_));
        }
        Request* request = flit->takeRequest();
        delete flit;
        if (request == nullptr)
        {
            output_.fatal(CALL_INFO, -1, "endpoint %u received empty untimed data\n",
                          static_cast<unsigned>(endpointId_));
        }
        untimedReceiveQueue_.push_back(request);
    }
}

void WormholeNetworkInterface::setup()
{
    initialized_ = true;
}

void WormholeNetworkInterface::finish()
{
    if (!assembling_.empty())
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u finished with %zu incomplete packets\n",
                      static_cast<unsigned>(endpointId_), assembling_.size());
    }
}

void WormholeNetworkInterface::handleRouterEvent(SST::Event* event, int laneValue)
{
    const std::uint32_t lane = static_cast<std::uint32_t>(laneValue);
    if (auto* burst = dynamic_cast<WormholeCreditBurstEvent*>(event))
    {
        if (burst->credits() == 0 || burst->creditsPerCycle() == 0)
        {
            delete burst;
            output_.fatal(CALL_INFO, -1, "endpoint %u received an invalid credit burst\n",
                          static_cast<unsigned>(endpointId_));
        }
        const std::uint64_t cycle = clockFactor_ == 0 ? 0 : getCurrentSimCycle() / clockFactor_;
        pendingCreditBursts_.push_back(PendingCreditBurst{
            cycle,
            burst->credits(),
            burst->creditsPerCycle(),
            lane,
        });
        delete burst;
        accrueRouterCredits(cycle);
        ensureClock();
        if (sendHandler_ != nullptr)
        {
            (*sendHandler_)(0);
        }
        return;
    }
    if (auto* credit = dynamic_cast<WormholeCreditEvent*>(event))
    {
        const std::uint64_t cycle = clockFactor_ == 0 ? 0 : getCurrentSimCycle() / clockFactor_;
        accrueRouterCredits(cycle);
        if (credit->credits() > routerBufferFlits_ - routerCredits_[lane])
        {
            delete credit;
            output_.fatal(CALL_INFO, -1, "endpoint %u received excess router credits\n",
                          static_cast<unsigned>(endpointId_));
        }
        injectionOrder_.returnedCredits(lane, credit->credits());
        routerCredits_[lane] += credit->credits();
        delete credit;
        ensureClock();
        if (sendHandler_ != nullptr)
        {
            (*sendHandler_)(0);
        }
        return;
    }
    if (auto* packet = dynamic_cast<WormholePacketBurstEvent*>(event))
    {
        if (packet->flitsPerCycle() != flitsPerCycle_ || packet->count() == 0)
        {
            delete packet;
            output_.fatal(CALL_INFO, -1, "endpoint %u received an invalid packet burst\n",
                          static_cast<unsigned>(endpointId_));
        }
        const std::uint64_t cycles = serializationCycles(packet->count());
        if (cycles == 1)
        {
            acceptPacketBurst(packet);
        }
        else
        {
            packetTailDeliveryLink_->send(cycles - 1, packet);
        }
        return;
    }
    auto* flit = dynamic_cast<WormholeFlitEvent*>(event);
    if (flit == nullptr)
    {
        delete event;
        output_.fatal(CALL_INFO, -1, "endpoint %u received invalid router event\n",
                      static_cast<unsigned>(endpointId_));
    }
    acceptFlit(flit, true, lane);
}

void WormholeNetworkInterface::handlePacketTailDelivery(SST::Event* event)
{
    auto* packet = dynamic_cast<WormholePacketBurstEvent*>(event);
    if (packet == nullptr)
    {
        delete event;
        output_.fatal(CALL_INFO, -1, "endpoint %u received an invalid packet-tail event\n",
                      static_cast<unsigned>(endpointId_));
    }
    acceptPacketBurst(packet);
}

bool WormholeNetworkInterface::clock(SST::Cycle_t cycle)
{
    const bool deferredCredits = !pendingCreditBursts_.empty();
    accrueRouterCredits(cycle);
    std::uint64_t occupancy = 0;
    for (std::uint32_t lane = 0; lane < txStreams_; ++lane)
    {
        occupancy += injectionQueues_[lane].size();
    }
    injectionQueueOccupancy_->addData(occupancy);
    if (occupancy == 0)
    {
        // Returning the final deferred credit may release destination ordering
        // even though the old packet has already emptied the injection queue.
        // Wake a blocked lane migration; do not require guest polling to retry.
        if (deferredCredits && sendHandler_ != nullptr)
        {
            (*sendHandler_)(0);
        }
        clockRegistered_ = activeLaneCount() != 0 || !pendingCreditBursts_.empty();
        return !clockRegistered_;
    }
    bool anyCredit = false;
    for (std::uint32_t lane = 0; lane < txStreams_; ++lane)
    {
        anyCredit |= routerCredits_[lane] != 0;
    }
    if (!anyCredit)
    {
        injectionCreditStallCycles_->addData(1);
        creditStallSleeping_ = true;
        clockStoppedAfterCycle_ = cycle;
        sleepingQueueOccupancy_ = occupancy;
        clockRegistered_ = false;
        return true;
    }

    for (std::uint32_t lane = 0; lane < txStreams_; ++lane)
    {
        auto& queue = injectionQueues_[lane];
        std::uint32_t sent = 0;
        while (sent < flitsPerCycle_ && routerCredits_[lane] != 0 && !queue.empty())
        {
            WormholeFlitEvent* flit = queue.front();
            queue.pop_front();
            if (flit->head())
            {
                activePacketInjectionTick_[lane] = getCurrentSimCycle();
            }
            flit->setInjectionTick(activePacketInjectionTick_[lane]);
            const bool tail = flit->tail();
            routerLinks_[lane]->send(flit);
            --routerCredits_[lane];
            ++sent;
            injectedFlits_->addData(1);
            if (tail)
            {
                activePacketInjectionTick_[lane] = 0;
            }
        }
    }
    if (sendHandler_ != nullptr)
    {
        (*sendHandler_)(0);
    }
    if (activeLaneCount() == 0 && pendingCreditBursts_.empty())
    {
        clockRegistered_ = false;
        return true;
    }
    return false;
}

void WormholeNetworkInterface::ensureClock()
{
    if (clockRegistered_ || (activeLaneCount() == 0 && pendingCreditBursts_.empty()))
    {
        return;
    }
    const SST::Cycle_t nextCycle = reregisterClock(clockTimeBase_, clockHandler_);
    if (creditStallSleeping_)
    {
        const std::uint64_t firstSkippedCycle = clockStoppedAfterCycle_ + 1;
        const std::uint64_t skippedCycles =
            nextCycle > firstSkippedCycle ? nextCycle - firstSkippedCycle : 0;
        if (skippedCycles != 0)
        {
            injectionQueueOccupancy_->addDataNTimes(skippedCycles, sleepingQueueOccupancy_);
            injectionCreditStallCycles_->addDataNTimes(skippedCycles, UINT64_C(1));
        }
        creditStallSleeping_ = false;
    }
    clockRegistered_ = true;
}

void WormholeNetworkInterface::accrueRouterCredits(std::uint64_t cycle)
{
    for (auto iterator = pendingCreditBursts_.begin(); iterator != pendingCreditBursts_.end();)
    {
        if (cycle < iterator->firstCycle)
        {
            ++iterator;
            continue;
        }
        const std::uint64_t elapsed = cycle - iterator->firstCycle + 1;
        const std::uint64_t available =
            std::min<std::uint64_t>(iterator->credits, elapsed * iterator->creditsPerCycle);
        if (available > routerBufferFlits_ - routerCredits_[iterator->lane])
        {
            output_.fatal(CALL_INFO, -1, "endpoint %u credit burst exceeded router capacity\n",
                          static_cast<unsigned>(endpointId_));
        }
        injectionOrder_.returnedCredits(iterator->lane, static_cast<std::uint32_t>(available));
        routerCredits_[iterator->lane] += static_cast<std::uint32_t>(available);
        iterator->credits -= static_cast<std::uint32_t>(available);
        if (iterator->credits == 0)
        {
            iterator = pendingCreditBursts_.erase(iterator);
            continue;
        }
        const std::uint64_t consumedCycles =
            (available + iterator->creditsPerCycle - 1) / iterator->creditsPerCycle;
        iterator->firstCycle += consumedCycles;
        ++iterator;
    }
}

std::uint32_t WormholeNetworkInterface::activeLaneCount() const noexcept
{
    std::uint32_t count = 0;
    for (std::uint32_t lane = 0; lane < txStreams_; ++lane)
    {
        count += injectionQueues_[lane].empty() ? 0U : 1U;
    }
    return count;
}

std::uint64_t WormholeNetworkInterface::serializationCycles(std::uint32_t flits) const
{
    return flits / flitsPerCycle_ + (flits % flitsPerCycle_ != 0 ? 1 : 0);
}

std::uint32_t WormholeNetworkInterface::flitCount(int bits) const
{
    if (bits <= 0)
    {
        return 0;
    }
    const std::uint64_t value = static_cast<std::uint64_t>(bits);
    const std::uint64_t count = value / 32 + (value % 32 != 0 ? 1 : 0);
    if (count > std::numeric_limits<std::uint32_t>::max())
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u packet is too large\n",
                      static_cast<unsigned>(endpointId_));
    }
    return static_cast<std::uint32_t>(count);
}

void WormholeNetworkInterface::acceptFlit(WormholeFlitEvent* flit, bool timed, std::uint32_t lane)
{
    if (timed && flitTrace_.is_open())
        flitTrace_ << getCurrentSimTimeNano()*1000 << ',' << flit->source() << ','
                   << flit->packetId() << ',' << flit->head() << ',' << flit->tail() << ',' << lane << '\n';
    if (flit->destination() != endpointId_)
    {
        delete flit;
        output_.fatal(CALL_INFO, -1, "endpoint %u received a misrouted flit\n",
                      static_cast<unsigned>(endpointId_));
    }
    receivedFlits_->addData(1);
    if (ejectionOccupancy_[lane] == routerBufferFlits_)
    {
        delete flit;
        output_.fatal(CALL_INFO, -1, "endpoint %u overflowed its %u-flit ejection buffer\n",
                      static_cast<unsigned>(endpointId_),
                      static_cast<unsigned>(routerBufferFlits_));
    }
    ++ejectionOccupancy_[lane];
    if (flit->head())
    {
        Request* request = flit->takeRequest();
        if (request == nullptr || !assembling_.emplace(std::make_pair(flit->source(), flit->packetId()), request).second)
        {
            delete request;
            delete flit;
            output_.fatal(CALL_INFO, -1, "endpoint %u received an invalid head flit\n",
                          static_cast<unsigned>(endpointId_));
        }
    }
    if (flit->tail())
    {
        const auto found = assembling_.find(std::make_pair(flit->source(), flit->packetId()));
        if (found == assembling_.end())
        {
            delete flit;
            output_.fatal(CALL_INFO, -1, "endpoint %u received a tail without a head\n",
                          static_cast<unsigned>(endpointId_));
        }
        Request* request = found->second;
        assembling_.erase(found);
        receiveQueue_.push_back(ReceivedPacket{
            request,
            flit->count(),
            lane,
        });
        completedPackets_->addData(1);
        if (timed && getCurrentSimCycle() >= flit->injectionTick())
        {
            packetNetworkLatency_->addData(getCurrentSimCycle() - flit->injectionTick());
        }
        if (receiveHandler_ != nullptr)
        {
            (*receiveHandler_)(0);
        }
    }
    delete flit;
}

void WormholeNetworkInterface::acceptPacketBurst(WormholePacketBurstEvent* packet, std::uint32_t lane)
{
    if (packet->destination() != endpointId_ || packet->count() == 0)
    {
        delete packet;
        output_.fatal(CALL_INFO, -1, "endpoint %u received a misrouted packet burst\n",
                      static_cast<unsigned>(endpointId_));
    }
    if (packet->count() > routerBufferFlits_ - ejectionOccupancy_[lane])
    {
        delete packet;
        output_.fatal(CALL_INFO, -1, "endpoint %u packet burst overflowed its ejection buffer\n",
                      static_cast<unsigned>(endpointId_));
    }
    Request* request = packet->takeRequest();
    if (request == nullptr)
    {
        delete packet;
        output_.fatal(CALL_INFO, -1, "endpoint %u received an empty packet burst\n",
                      static_cast<unsigned>(endpointId_));
    }
    ejectionOccupancy_[lane] += packet->count();
    receivedFlits_->addDataNTimes(packet->count(), UINT64_C(1));
    receiveQueue_.push_back(ReceivedPacket{request, packet->count(), lane});
    completedPackets_->addData(1);
    if (getCurrentSimCycle() >= packet->injectionTick())
    {
        packetNetworkLatency_->addData(getCurrentSimCycle() - packet->injectionTick());
    }
    if (receiveHandler_ != nullptr)
    {
        (*receiveHandler_)(0);
    }
    delete packet;
}

} // namespace Mittens
} // namespace SST
