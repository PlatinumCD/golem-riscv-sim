#ifndef SST_MITTENS_WORMHOLE_NETWORK_INTERFACE_H
#define SST_MITTENS_WORMHOLE_NETWORK_INTERFACE_H

#include <array>
#include <map>
#include <cstdint>
#include <deque>
#include <fstream>
#include <unordered_map>

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>
#include <sst/core/unitAlgebra.h>

#include "wormholeEvents.h"
#include "injectionOrder.h"
#include "../configuration/networkConfiguration.h"

namespace SST
{
namespace Mittens
{

class WormholeNetworkInterface final : public SST::Interfaces::SimpleNetwork
{
  public:
    SST_ELI_REGISTER_SUBCOMPONENT(WormholeNetworkInterface, "mittens", "wormholeNIC",
                                  SST_ELI_ELEMENT_VERSION(0, 1, 0),
                                  "SimpleNetwork endpoint for the Mittens wormhole router",
                                  SST::Interfaces::SimpleNetwork)

    SST_ELI_DOCUMENT_PARAMS(
        {"endpoint_id", "Linear endpoint identifier"}, {"network_size", "Number of endpoints"},
        {"clock", "Physical-link transfer clock", "1GHz"},
        {"receive_flit_trace_path", "Optional timed NIC-arrival CSV; empty disables", ""},
        {"link_width_bits", "Physical link width per cycle", "32"},
        {"router_buffer_flits", "Initial credits per local TX lane and shared ejection capacity",
         "32"},
        {"injection_buffer_flits", "Maximum queued injection flits per local TX lane", "64"},
        {"packet_burst_coalescing",
         "Accepted compatibility option; NIC injection remains flit-streamed", "false"},
        {"mesh_width", "Physical mesh width", "0"}, {"mesh_height", "Physical mesh height", "0"},
        {"rx_streams", "Independent ejection lanes: 1, 2, or 4", "1"},
        {"tx_streams", "Independent local TX injection lanes: 1, 2, or 4", "1"},
        {"verbose", "Diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS({"router_port",
                            "Compatibility alias for local lane 0",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"router_port0",
                            "Local injection lane 0 and ejection link",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"router_port1",
                            "Local injection lane 1",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"router_port2",
                            "Local injection lane 2",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"router_port3",
                            "Local injection lane 3",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"injected_flits", "Flits injected into the local router", "flits", 1},
        {"received_flits", "Flits consumed from the local router", "flits", 1},
        {"completed_packets", "Complete packets delivered to the endpoint", "packets", 1},
        {"injection_credit_stall_cycles", "Active cycles blocked without router credits", "cycles",
         1},
        {"injection_queue_occupancy", "Injection occupancy sampled in active cycles", "flits", 1},
        {"packet_network_latency", "Head-injection through tail-arrival latency", "ticks", 1})

    WormholeNetworkInterface(SST::ComponentId_t id, SST::Params& params, int virtualNetworks);
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
    struct ReceivedPacket
    {
        Request* request = nullptr;
        std::uint32_t flits = 0;
        std::uint32_t lane = 0;
    };

    struct PendingCreditBurst
    {
        std::uint64_t firstCycle = 0;
        std::uint32_t credits = 0;
        std::uint32_t creditsPerCycle = 1;
        std::uint32_t lane = 0;
    };

    void handleRouterEvent(SST::Event* event, int lane);
    void handlePacketTailDelivery(SST::Event* event);
    bool clock(SST::Cycle_t cycle);
    void ensureClock();
    void accrueRouterCredits(std::uint64_t cycle);
    std::uint64_t serializationCycles(std::uint32_t flits) const;
    std::uint32_t flitCount(int bits) const;
    void acceptFlit(WormholeFlitEvent* flit, bool timed, std::uint32_t lane = 0);
    void acceptPacketBurst(WormholePacketBurstEvent* packet, std::uint32_t lane = 0);
    std::uint32_t activeLaneCount() const noexcept;

    const NetworkInterfaceConfiguration configuration_;
    std::uint32_t endpointId_ = 0;
    std::uint32_t networkSize_ = 0;
    std::uint32_t txStreams_ = 1;
    std::uint32_t rxStreams_ = 1;
    std::uint32_t flitsPerCycle_ = 1;
    std::uint32_t routerBufferFlits_ = 0;
    std::array<std::uint32_t, 4> routerCredits_{};
    std::uint32_t injectionBufferFlits_ = 64;
    std::uint64_t clockFactor_ = 0;
    std::uint64_t nextPacketId_ = 0;
    std::array<std::uint64_t, 4> activePacketInjectionTick_{};
    bool initialized_ = false;
    bool clockRegistered_ = false;
    bool creditStallSleeping_ = false;
    std::uint64_t clockStoppedAfterCycle_ = 0;
    std::uint64_t sleepingQueueOccupancy_ = 0;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
    SST::Output output_;
    SST::UnitAlgebra linkBandwidth_;
    SST::TimeConverter clockTimeBase_;
    std::array<SST::Link*, 4> routerLinks_{};
    SST::Link* packetTailDeliveryLink_ = nullptr;
    HandlerBase* receiveHandler_ = nullptr;
    std::ofstream flitTrace_;
    HandlerBase* sendHandler_ = nullptr;
    std::array<std::deque<WormholeFlitEvent*>, 4> injectionQueues_;
    InjectionOrder injectionOrder_;
    std::deque<PendingCreditBurst> pendingCreditBursts_;
    std::deque<ReceivedPacket> receiveQueue_;
    std::deque<Request*> untimedReceiveQueue_;
    // Packet IDs are allocated per sender, not globally. Concurrent ejection
    // may have the same packet ID from several sources in flight together.
    std::map<std::pair<std::uint32_t, std::uint64_t>, Request*> assembling_;
    std::array<std::uint32_t, 4> ejectionOccupancy_{};
    SST::Statistics::Statistic<std::uint64_t>* injectedFlits_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* receivedFlits_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* completedPackets_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* injectionCreditStallCycles_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* injectionQueueOccupancy_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* packetNetworkLatency_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
