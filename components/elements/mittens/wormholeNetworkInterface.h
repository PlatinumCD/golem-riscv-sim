#ifndef SST_MITTENS_WORMHOLE_NETWORK_INTERFACE_H
#define SST_MITTENS_WORMHOLE_NETWORK_INTERFACE_H

#include <cstdint>
#include <deque>
#include <unordered_map>

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>
#include <sst/core/unitAlgebra.h>

#include "wormholeEvents.h"

namespace SST {
namespace Mittens {

class WormholeNetworkInterface final :
    public SST::Interfaces::SimpleNetwork
{
  public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        WormholeNetworkInterface,
        "mittens",
        "wormholeNIC",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "SimpleNetwork endpoint for the Mittens wormhole router",
        SST::Interfaces::SimpleNetwork)

    SST_ELI_DOCUMENT_PARAMS(
        {"endpoint_id", "Linear endpoint identifier"},
        {"network_size", "Number of endpoints"},
        {"clock", "Physical-link transfer clock", "1GHz"},
        {"link_width_bits", "Physical link width per cycle", "32"},
        {"router_buffer_flits", "Credits initially available at the attached router", "32"},
        {"injection_buffer_flits", "Maximum queued injection flits", "64"},
        {"verbose", "Diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"router_port", "Connection to the local wormhole router", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"injected_flits", "Flits injected into the local router", "flits", 1},
        {"received_flits", "Flits consumed from the local router", "flits", 1},
        {"completed_packets", "Complete packets delivered to the endpoint", "packets", 1},
        {"injection_credit_stall_cycles", "Active cycles blocked without router credits", "cycles", 1},
        {"injection_queue_occupancy", "Injection occupancy sampled in active cycles", "flits", 1},
        {"packet_network_latency", "Head-injection through tail-arrival latency", "ticks", 1})

    WormholeNetworkInterface(
        SST::ComponentId_t id,
        SST::Params& params,
        int virtualNetworks);
    ~WormholeNetworkInterface() override;

    void sendUntimedData(Request* request) override;
    Request* recvUntimedData() override;
    bool send(Request* request, int virtualNetwork) override;
    Request* recv(int virtualNetwork) override;
    bool spaceToSend(int virtualNetwork, int bits) override;
    bool requestToReceive(int virtualNetwork) override;
    void setNotifyOnReceive(HandlerBase* handler) override;
    void setNotifyOnSend(HandlerBase* handler) override;
    bool isNetworkInitialized() const override;
    nid_t getEndpointID() const override;
    const SST::UnitAlgebra& getLinkBW() const override;
    void init(unsigned phase) override;
    void setup() override;
    void finish() override;

  private:
    struct ReceivedPacket {
        Request* request = nullptr;
        std::uint32_t flits = 0;
    };

    void handleRouterEvent(SST::Event* event);
    bool clock(SST::Cycle_t cycle);
    void ensureClock();
    std::uint32_t flitCount(int bits) const;
    void acceptFlit(WormholeFlitEvent* flit, bool timed);

    std::uint32_t endpointId_ = 0;
    std::uint32_t networkSize_ = 0;
    std::uint32_t flitsPerCycle_ = 1;
    std::uint32_t routerBufferFlits_ = 0;
    std::uint32_t routerCredits_ = 0;
    std::uint32_t injectionBufferFlits_ = 64;
    std::uint64_t nextPacketId_ = 0;
    std::uint64_t activePacketInjectionTick_ = 0;
    bool initialized_ = false;
    bool clockRegistered_ = false;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
    SST::Output output_;
    SST::UnitAlgebra linkBandwidth_;
    SST::TimeConverter clockTimeBase_;
    SST::Link* routerLink_ = nullptr;
    HandlerBase* receiveHandler_ = nullptr;
    HandlerBase* sendHandler_ = nullptr;
    std::deque<WormholeFlitEvent*> injectionQueue_;
    std::deque<ReceivedPacket> receiveQueue_;
    std::deque<Request*> untimedReceiveQueue_;
    std::unordered_map<std::uint64_t, Request*> assembling_;
    std::uint32_t ejectionOccupancy_ = 0;
    SST::Statistics::Statistic<std::uint64_t>* injectedFlits_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* receivedFlits_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* completedPackets_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>*
        injectionCreditStallCycles_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>*
        injectionQueueOccupancy_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* packetNetworkLatency_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
