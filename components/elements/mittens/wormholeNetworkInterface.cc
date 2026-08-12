#include "wormholeNetworkInterface.h"

#include <limits>
#include <string>
#include <utility>

namespace SST {
namespace Mittens {

WormholeNetworkInterface::WormholeNetworkInterface(
    SST::ComponentId_t id,
    SST::Params& params,
    int virtualNetworks) :
    SST::Interfaces::SimpleNetwork(id),
    endpointId_(params.find<std::uint32_t>("endpoint_id")),
    networkSize_(params.find<std::uint32_t>("network_size")),
    flitsPerCycle_(
        params.find<std::uint32_t>("link_width_bits", 32) / 32),
    routerBufferFlits_(
        params.find<std::uint32_t>("router_buffer_flits", 32)),
    routerCredits_(routerBufferFlits_),
    injectionBufferFlits_(
        params.find<std::uint32_t>("injection_buffer_flits", 64)),
    output_(
        "mittens-wormhole-nic: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT),
    linkBandwidth_("1b/s")
{
    const std::uint32_t linkWidth =
        params.find<std::uint32_t>("link_width_bits", 32);
    if (virtualNetworks != 1) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u supports exactly one virtual network\n",
            static_cast<unsigned>(endpointId_));
    }
    if (networkSize_ == 0 || endpointId_ >= networkSize_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u is outside network size %u\n",
            static_cast<unsigned>(endpointId_),
            static_cast<unsigned>(networkSize_));
    }
    if (linkWidth == 0 || linkWidth % 32 != 0 ||
        routerBufferFlits_ == 0 || injectionBufferFlits_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u has invalid link or buffer parameters\n",
            static_cast<unsigned>(endpointId_));
    }

    const std::string clock =
        params.find<std::string>("clock", "1GHz");
    clockTimeBase_ = getTimeConverter(clock);
    clockHandler_ = new SST::Clock::Handler<
        WormholeNetworkInterface,
        &WormholeNetworkInterface::clock>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
    SST::UnitAlgebra frequency(clock);
    linkBandwidth_ = frequency *
        SST::UnitAlgebra(std::to_string(linkWidth) + "b");
    routerLink_ = configureLink(
        "router_port",
        new SST::Event::Handler<
            WormholeNetworkInterface,
            &WormholeNetworkInterface::handleRouterEvent>(this));
    if (routerLink_ == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u requires router_port\n",
            static_cast<unsigned>(endpointId_));
    }

    injectedFlits_ = registerStatistic<std::uint64_t>("injected_flits");
    receivedFlits_ = registerStatistic<std::uint64_t>("received_flits");
    completedPackets_ =
        registerStatistic<std::uint64_t>("completed_packets");
    injectionCreditStallCycles_ = registerStatistic<std::uint64_t>(
        "injection_credit_stall_cycles");
    injectionQueueOccupancy_ = registerStatistic<std::uint64_t>(
        "injection_queue_occupancy");
    packetNetworkLatency_ = registerStatistic<std::uint64_t>(
        "packet_network_latency");
}

WormholeNetworkInterface::~WormholeNetworkInterface()
{
    delete receiveHandler_;
    delete sendHandler_;
    while (!injectionQueue_.empty()) {
        delete injectionQueue_.front();
        injectionQueue_.pop_front();
    }
    while (!receiveQueue_.empty()) {
        delete receiveQueue_.front().request;
        receiveQueue_.pop_front();
    }
    while (!untimedReceiveQueue_.empty()) {
        delete untimedReceiveQueue_.front();
        untimedReceiveQueue_.pop_front();
    }
    for (auto& [packet, request] : assembling_) {
        static_cast<void>(packet);
        delete request;
    }
}

void WormholeNetworkInterface::sendUntimedData(Request* request)
{
    if (request == nullptr) {
        return;
    }
    request->src = endpointId_;
    request->vn = 0;
    routerLink_->sendUntimedData(new WormholeFlitEvent(
        nextPacketId_++,
        endpointId_,
        static_cast<std::uint32_t>(request->dest),
        0,
        0,
        1,
        0,
        request));
}

SST::Interfaces::SimpleNetwork::Request*
WormholeNetworkInterface::recvUntimedData()
{
    if (untimedReceiveQueue_.empty()) {
        return nullptr;
    }
    Request* request = untimedReceiveQueue_.front();
    untimedReceiveQueue_.pop_front();
    return request;
}

bool WormholeNetworkInterface::send(
    Request* request,
    int virtualNetwork)
{
    if (request == nullptr || virtualNetwork != 0 || !initialized_) {
        return false;
    }
    if (request->dest < 0 ||
        request->dest >= static_cast<nid_t>(networkSize_)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u received invalid destination %lld\n",
            static_cast<unsigned>(endpointId_),
            static_cast<long long>(request->dest));
    }
    const std::uint32_t count = flitCount(request->size_in_bits);
    if (injectionQueue_.size() + count > injectionBufferFlits_) {
        return false;
    }

    request->src = endpointId_;
    request->vn = virtualNetwork;
    const std::uint64_t packetId = nextPacketId_++;
    for (std::uint32_t index = 0; index < count; ++index) {
        injectionQueue_.push_back(new WormholeFlitEvent(
            packetId,
            endpointId_,
            static_cast<std::uint32_t>(request->dest),
            static_cast<std::uint32_t>(virtualNetwork),
            index,
            count,
            0,
            index == 0 ? request : nullptr));
    }
    ensureClock();
    return true;
}

SST::Interfaces::SimpleNetwork::Request*
WormholeNetworkInterface::recv(int virtualNetwork)
{
    if (virtualNetwork != 0 || receiveQueue_.empty()) {
        return nullptr;
    }
    const ReceivedPacket packet = receiveQueue_.front();
    receiveQueue_.pop_front();
    if (packet.flits > ejectionOccupancy_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u has invalid ejection occupancy\n",
            static_cast<unsigned>(endpointId_));
    }
    ejectionOccupancy_ -= packet.flits;
    routerLink_->send(new WormholeCreditEvent(packet.flits));
    return packet.request;
}

bool WormholeNetworkInterface::spaceToSend(
    int virtualNetwork,
    int bits)
{
    if (virtualNetwork != 0 || bits <= 0 || !initialized_) {
        return false;
    }
    const std::uint32_t count = flitCount(bits);
    return injectionQueue_.size() + count <= injectionBufferFlits_;
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

SST::Interfaces::SimpleNetwork::nid_t
WormholeNetworkInterface::getEndpointID() const
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
    while (SST::Event* raw = routerLink_->recvUntimedData()) {
        auto* flit = dynamic_cast<WormholeFlitEvent*>(raw);
        if (flit == nullptr) {
            delete raw;
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received invalid untimed data\n",
                static_cast<unsigned>(endpointId_));
        }
        Request* request = flit->takeRequest();
        delete flit;
        if (request == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received empty untimed data\n",
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
    if (!assembling_.empty()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u finished with %zu incomplete packets\n",
            static_cast<unsigned>(endpointId_),
            assembling_.size());
    }
}

void WormholeNetworkInterface::handleRouterEvent(SST::Event* event)
{
    if (auto* credit = dynamic_cast<WormholeCreditEvent*>(event)) {
        if (credit->credits() >
            routerBufferFlits_ - routerCredits_) {
            delete credit;
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received excess router credits\n",
                static_cast<unsigned>(endpointId_));
        }
        routerCredits_ += credit->credits();
        delete credit;
        ensureClock();
        if (sendHandler_ != nullptr) {
            (*sendHandler_)(0);
        }
        return;
    }
    auto* flit = dynamic_cast<WormholeFlitEvent*>(event);
    if (flit == nullptr) {
        delete event;
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u received invalid router event\n",
            static_cast<unsigned>(endpointId_));
    }
    acceptFlit(flit, true);
}

bool WormholeNetworkInterface::clock(SST::Cycle_t)
{
    injectionQueueOccupancy_->addData(injectionQueue_.size());
    if (injectionQueue_.empty()) {
        clockRegistered_ = false;
        return true;
    }
    if (routerCredits_ == 0) {
        injectionCreditStallCycles_->addData(1);
        return false;
    }

    std::uint32_t sent = 0;
    while (sent < flitsPerCycle_ && routerCredits_ != 0 &&
           !injectionQueue_.empty()) {
        WormholeFlitEvent* flit = injectionQueue_.front();
        injectionQueue_.pop_front();
        if (flit->head()) {
            activePacketInjectionTick_ = getCurrentSimCycle();
        }
        flit->setInjectionTick(activePacketInjectionTick_);
        const bool tail = flit->tail();
        routerLink_->send(flit);
        --routerCredits_;
        ++sent;
        injectedFlits_->addData(1);
        if (tail) {
            activePacketInjectionTick_ = 0;
        }
    }
    if (sendHandler_ != nullptr) {
        (*sendHandler_)(0);
    }
    if (injectionQueue_.empty()) {
        clockRegistered_ = false;
        return true;
    }
    return false;
}

void WormholeNetworkInterface::ensureClock()
{
    if (clockRegistered_ || injectionQueue_.empty()) {
        return;
    }
    reregisterClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

std::uint32_t WormholeNetworkInterface::flitCount(int bits) const
{
    if (bits <= 0) {
        return 0;
    }
    const std::uint64_t value = static_cast<std::uint64_t>(bits);
    const std::uint64_t count = value / 32 + (value % 32 != 0 ? 1 : 0);
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u packet is too large\n",
            static_cast<unsigned>(endpointId_));
    }
    return static_cast<std::uint32_t>(count);
}

void WormholeNetworkInterface::acceptFlit(
    WormholeFlitEvent* flit,
    bool timed)
{
    if (flit->destination() != endpointId_) {
        delete flit;
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u received a misrouted flit\n",
            static_cast<unsigned>(endpointId_));
    }
    receivedFlits_->addData(1);
    if (ejectionOccupancy_ == routerBufferFlits_) {
        delete flit;
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u overflowed its %u-flit ejection buffer\n",
            static_cast<unsigned>(endpointId_),
            static_cast<unsigned>(routerBufferFlits_));
    }
    ++ejectionOccupancy_;
    if (flit->head()) {
        Request* request = flit->takeRequest();
        if (request == nullptr ||
            !assembling_.emplace(flit->packetId(), request).second) {
            delete request;
            delete flit;
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received an invalid head flit\n",
                static_cast<unsigned>(endpointId_));
        }
    }
    if (flit->tail()) {
        const auto found = assembling_.find(flit->packetId());
        if (found == assembling_.end()) {
            delete flit;
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received a tail without a head\n",
                static_cast<unsigned>(endpointId_));
        }
        Request* request = found->second;
        assembling_.erase(found);
        receiveQueue_.push_back(ReceivedPacket{
            request,
            flit->count(),
        });
        completedPackets_->addData(1);
        if (timed && getCurrentSimCycle() >= flit->injectionTick()) {
            packetNetworkLatency_->addData(
                getCurrentSimCycle() - flit->injectionTick());
        }
        if (receiveHandler_ != nullptr) {
            (*receiveHandler_)(0);
        }
    }
    delete flit;
}

} // namespace Mittens
} // namespace SST
