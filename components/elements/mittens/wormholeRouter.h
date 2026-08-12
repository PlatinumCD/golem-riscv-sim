#ifndef SST_MITTENS_WORMHOLE_ROUTER_H
#define SST_MITTENS_WORMHOLE_ROUTER_H

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/statapi/stataccumulator.h>
#include <sst/core/timeConverter.h>

#include "wormholeEvents.h"

namespace SST {
namespace Mittens {

class WormholeRouter final : public SST::Component
{
  public:
    static constexpr std::uint32_t East = 0;
    static constexpr std::uint32_t West = 1;
    static constexpr std::uint32_t South = 2;
    static constexpr std::uint32_t North = 3;
    static constexpr std::uint32_t Local = 4;
    static constexpr std::uint32_t PortCount = 5;

    SST_ELI_REGISTER_COMPONENT(
        WormholeRouter,
        "mittens",
        "wormholeRouter",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Flit-streaming, credit-controlled mesh router",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        {"id", "Linear router identifier"},
        {"mesh_width", "Number of router columns"},
        {"mesh_height", "Number of router rows"},
        {"clock", "Router and physical-link transfer clock", "1GHz"},
        {"link_width_bits", "Physical output width per cycle", "32"},
        {"input_buffer_flits", "Input capacity for each port", "32"},
        {"pipeline_cycles", "Head-flit route and switch pipeline latency", "3"},
        {"verbose", "Diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"port0", "East router link", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}},
        {"port1", "West router link", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}},
        {"port2", "South router link", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}},
        {"port3", "North router link", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}},
        {"port4", "Local endpoint link", {"mittens.WormholeFlitEvent", "mittens.WormholeCreditEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"flits_forwarded", "Flits transmitted through an output", "flits", 1},
        {"packets_forwarded", "Tail flits transmitted through an output", "packets", 1},
        {"input_buffer_full_cycles", "Cycles an input buffer is full", "cycles", 1},
        {"switch_arbitration_stall_cycles", "Ready packet-head cycles blocked at an output", "cycles", 1},
        {"output_credit_stall_cycles", "Cycles a requested output has no downstream credit", "cycles", 1},
        {"output_link_busy_cycles", "Cycles an output transmits at least one flit", "cycles", 1},
        {"input_buffer_occupancy", "Input-buffer occupancy sampled in active cycles", "flits", 1})

    WormholeRouter(SST::ComponentId_t id, SST::Params& params);
    ~WormholeRouter() override;

    void init(unsigned phase) override;

  private:
    struct BufferedFlit {
        WormholeFlitEvent* event = nullptr;
        std::uint64_t readyCycle = 0;
    };

    using Statistic = SST::Statistics::Statistic<std::uint64_t>;

    void handlePortEvent(SST::Event* event, int port);
    bool clock(SST::Cycle_t cycle);
    void ensureClock();
    bool hasWork() const noexcept;
    std::uint32_t route(std::uint32_t destination) const;
    void sendFromInput(
        std::uint32_t input,
        std::uint32_t output,
        std::uint32_t budget,
        std::uint64_t cycle);
    void forwardUntimed(WormholeFlitEvent* flit);
    std::string portName(std::uint32_t port) const;

    std::uint32_t routerId_ = 0;
    std::uint32_t meshWidth_ = 0;
    std::uint32_t meshHeight_ = 0;
    std::uint32_t inputBufferFlits_ = 32;
    std::uint32_t pipelineCycles_ = 3;
    std::uint32_t flitsPerCycle_ = 1;
    std::uint64_t clockFactor_ = 0;
    bool clockRegistered_ = false;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
    SST::Output output_;
    SST::TimeConverter clockTimeBase_;
    std::array<SST::Link*, PortCount> links_{};
    std::array<std::deque<BufferedFlit>, PortCount> inputs_{};
    std::array<std::uint32_t, PortCount> outputCredits_{};
    std::array<std::optional<std::uint32_t>, PortCount> outputOwners_{};
    std::array<std::optional<std::uint32_t>, PortCount> inputRoutes_{};
    std::array<std::uint32_t, PortCount> roundRobin_{};
    std::array<Statistic*, PortCount> flitsForwarded_{};
    std::array<Statistic*, PortCount> packetsForwarded_{};
    std::array<Statistic*, PortCount> inputBufferFullCycles_{};
    std::array<Statistic*, PortCount> switchArbitrationStallCycles_{};
    std::array<Statistic*, PortCount> outputCreditStallCycles_{};
    std::array<Statistic*, PortCount> outputLinkBusyCycles_{};
    std::array<Statistic*, PortCount> inputBufferOccupancy_{};
};

} // namespace Mittens
} // namespace SST

#endif
