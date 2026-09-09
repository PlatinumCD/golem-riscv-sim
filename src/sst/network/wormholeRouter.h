#ifndef SST_MITTENS_WORMHOLE_ROUTER_H
#define SST_MITTENS_WORMHOLE_ROUTER_H

#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <optional>
#include <map>
#include <string>

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/statapi/stataccumulator.h>
#include <sst/core/timeConverter.h>

#include "wormholeEvents.h"
#include "../configuration/networkConfiguration.h"

namespace SST
{
namespace Mittens
{

class WormholeRouter final : public SST::Component
{
  public:
    static constexpr std::uint32_t East = 0;
    static constexpr std::uint32_t West = 1;
    static constexpr std::uint32_t South = 2;
    static constexpr std::uint32_t North = 3;
    static constexpr std::uint32_t Local = 4;
    static constexpr std::uint32_t OutputCount = 8;
    static constexpr std::uint32_t MaximumLocalInputs = 4;
    static constexpr std::uint32_t InputCount = 4 + MaximumLocalInputs;
    // Public compatibility alias: outputs, not the multi-lane input count.
    static constexpr std::uint32_t PortCount = OutputCount;

    SST_ELI_REGISTER_COMPONENT(WormholeRouter, "mittens", "wormholeRouter",
                               SST_ELI_ELEMENT_VERSION(0, 1, 0),
                               "Flit-streaming, credit-controlled mesh router",
                               COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS({"id", "Linear router identifier"},
                            {"mesh_width", "Number of router columns"},
                            {"mesh_height", "Number of router rows"},
                            {"clock", "Router and physical-link transfer clock", "1GHz"},
                            {"link_width_bits", "Physical output width per cycle", "32"},
                            {"input_buffer_flits", "Input capacity for each port", "32"},
                            {"pipeline_cycles", "Head-flit route and switch pipeline latency", "3"},
                            {"packet_burst_coalescing",
                             "Experimental contiguous packet reservation path", "false"},
                            {"route_overrides", "Experimental destination:output pairs; E=0,W=1,S=2,N=3. Empty preserves XY.", ""},
                            {"loopback_injection_output", "Experimental self-addressed local injection direction; -1 disables. Returning cardinal traffic ejects locally.", "-1"},
                            {"rx_streams", "Independent local ejection lanes: 1, 2, or 4", "1"},
                            {"tx_streams", "Independent local injection lanes: 1, 2, or 4", "1"},
                            {"flit_trace_path", "Optional CSV of actual output flit departures", ""},
                            {"packet_trace_path", "Optional flit-mode packet grant/tail timing CSV", ""},
                            {"verbose", "Diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS({"port0",
                            "East router link",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port1",
                            "West router link",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port2",
                            "South router link",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port3",
                            "North router link",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port4",
                            "Local endpoint lane 0",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port5",
                            "Local endpoint lane 1",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port6",
                            "Local endpoint lane 2",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}},
                           {"port7",
                            "Local endpoint lane 3",
                            {"mittens.WormholeFlitEvent", "mittens.WormholePacketBurstEvent",
                             "mittens.WormholeCreditEvent", "mittens.WormholeCreditBurstEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"flits_forwarded", "Flits transmitted through an output", "flits", 1},
        {"packets_forwarded", "Tail flits transmitted through an output", "packets", 1},
        {"input_buffer_full_cycles", "Cycles an input buffer is full", "cycles", 1},
        {"switch_arbitration_stall_cycles", "Ready packet-head cycles blocked at an output",
         "cycles", 1},
        {"output_credit_stall_cycles", "Cycles a requested output has no downstream credit",
         "cycles", 1},
        {"output_link_busy_cycles", "Cycles an output transmits at least one flit", "cycles", 1},
        {"cardinal_outputs_active",
         "Cardinal output links simultaneously active in a source-router cycle", "links", 1},
        {"input_buffer_occupancy", "Input-buffer occupancy sampled in active cycles", "flits", 1})

    WormholeRouter(SST::ComponentId_t id, SST::Params& params);
    ~WormholeRouter() override;

    void init(unsigned phase) override;

  private:
    std::ofstream flitTrace_;
    std::ofstream packetTrace_;
    std::map<std::uint32_t, std::uint32_t> routeOverrides_;
    struct BufferedFlit
    {
        WormholeFlitEvent* event = nullptr;
        std::uint64_t readyCycle = 0;
    };

    struct IncomingPacketBurst
    {
        std::uint64_t packetId = 0;
        std::uint32_t source = 0;
        std::uint32_t destination = 0;
        std::uint32_t virtualNetwork = 0;
        std::uint32_t count = 0;
        std::uint32_t flitsPerCycle = 1;
        std::uint32_t nextIndex = 0;
        std::uint64_t headArrivalCycle = 0;
        std::uint64_t injectionTick = 0;
        SST::Interfaces::SimpleNetwork::Request* request = nullptr;
    };

    struct PendingCreditBurst
    {
        std::uint64_t firstCycle = 0;
        std::uint32_t credits = 0;
        std::uint32_t creditsPerCycle = 1;
    };

    struct SwitchDecision
    {
        std::array<std::optional<std::uint32_t>, OutputCount> selected{};
        std::array<bool, OutputCount> requested{};
        std::array<std::uint32_t, OutputCount> arbitrationStalls{};
    };

    using Statistic = SST::Statistics::Statistic<std::uint64_t>;

    void handlePortEvent(SST::Event* event, int port);
    void handleBurstRelease(SST::Event* event);
    bool clock(SST::Cycle_t cycle);
    void ensureClock();
    void materializeInputBursts(std::uint32_t input, std::uint64_t cycle);
    void accrueOutputCredits(std::uint32_t output, std::uint64_t cycle);
    bool trySendPacketBurst(std::uint32_t input, std::uint32_t output,
                            std::uint32_t arbitrationStalls, std::uint64_t cycle);
    std::uint64_t serializationCycles(std::uint32_t flits) const;
    SwitchDecision selectInputs(std::uint64_t cycle) const;
    bool beginCreditStallSleep(std::uint64_t cycle);
    void accountCreditStallSleep(SST::Cycle_t nextCycle);
    bool hasWork() const noexcept;
    std::uint32_t route(std::uint32_t destination, std::uint32_t input = 0) const;
    int loopbackInjectionOutput_ = -1;
    bool sendFromInput(std::uint32_t input, std::uint32_t output, std::uint32_t budget,
                       std::uint64_t cycle);
    void forwardUntimed(WormholeFlitEvent* flit);
    std::string portName(std::uint32_t port) const;

    const RouterConfiguration configuration_;
    std::uint32_t routerId_ = 0;
    std::uint32_t meshWidth_ = 0;
    std::uint32_t meshHeight_ = 0;
    std::uint32_t inputBufferFlits_ = 32;
    std::uint32_t pipelineCycles_ = 3;
    std::uint32_t flitsPerCycle_ = 1;
    bool packetBurstCoalescing_ = false;
    std::uint32_t txStreams_ = 1;
    std::uint32_t rxStreams_ = 1;
    std::uint64_t clockFactor_ = 0;
    bool clockRegistered_ = false;
    bool creditStallSleeping_ = false;
    std::uint64_t clockStoppedAfterCycle_ = 0;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
    SST::Link* burstReleaseLink_ = nullptr;
    SST::Output output_;
    SST::TimeConverter clockTimeBase_;
    std::array<SST::Link*, InputCount> links_{};
    std::array<std::deque<BufferedFlit>, InputCount> inputs_{};
    std::array<std::deque<IncomingPacketBurst>, InputCount> incomingPacketBursts_{};
    std::array<std::deque<PendingCreditBurst>, OutputCount> pendingCreditBursts_{};
    std::array<std::uint32_t, InputCount> inputReservedFlits_{};
    std::array<std::uint32_t, OutputCount> outputCredits_{};
    std::array<std::optional<std::uint32_t>, OutputCount> outputOwners_{};
    std::array<std::optional<std::uint32_t>, InputCount> inputRoutes_{};
    std::array<bool, OutputCount> burstOutputActive_{};
    std::array<std::uint64_t, OutputCount> burstTailCycle_{};
    std::array<std::uint32_t, OutputCount> roundRobin_{};
    std::array<std::uint64_t, InputCount> sleepingOccupancy_{};
    std::array<std::uint32_t, OutputCount> sleepingArbitrationStalls_{};
    std::array<bool, OutputCount> sleepingCreditStalls_{};
    std::array<Statistic*, OutputCount> flitsForwarded_{};
    std::array<Statistic*, OutputCount> packetsForwarded_{};
    std::array<Statistic*, InputCount> inputBufferFullCycles_{};
    std::array<Statistic*, OutputCount> switchArbitrationStallCycles_{};
    std::array<Statistic*, OutputCount> outputCreditStallCycles_{};
    std::array<Statistic*, OutputCount> outputLinkBusyCycles_{};
    std::array<Statistic*, InputCount> inputBufferOccupancy_{};
    Statistic* cardinalOutputsActive_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
