#include "wormholeRouter.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace SST {
namespace Mittens {

WormholeRouter::WormholeRouter(
    SST::ComponentId_t id,
    SST::Params& params) :
    SST::Component(id),
    routerId_(params.find<std::uint32_t>("id")),
    meshWidth_(params.find<std::uint32_t>("mesh_width")),
    meshHeight_(params.find<std::uint32_t>("mesh_height")),
    inputBufferFlits_(
        params.find<std::uint32_t>("input_buffer_flits", 32)),
    pipelineCycles_(params.find<std::uint32_t>("pipeline_cycles", 3)),
    flitsPerCycle_(
        params.find<std::uint32_t>("link_width_bits", 32) / 32),
    output_(
        "mittens-wormhole-router: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT)
{
    const std::uint32_t linkWidth =
        params.find<std::uint32_t>("link_width_bits", 32);
    if (meshWidth_ == 0 || meshHeight_ == 0 ||
        routerId_ >= meshWidth_ * meshHeight_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u has invalid %ux%u mesh coordinates\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(meshWidth_),
            static_cast<unsigned>(meshHeight_));
    }
    if (linkWidth == 0 || linkWidth % 32 != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u link_width_bits must be a positive multiple of 32\n",
            static_cast<unsigned>(routerId_));
    }
    if (inputBufferFlits_ == 0 || pipelineCycles_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u requires positive buffer and pipeline sizes\n",
            static_cast<unsigned>(routerId_));
    }

    clockTimeBase_ = getTimeConverter(
        params.find<std::string>("clock", "1GHz"));
    clockFactor_ = clockTimeBase_.getFactor();
    clockHandler_ = new SST::Clock::Handler<
        WormholeRouter,
        &WormholeRouter::clock>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
    for (std::uint32_t port = 0; port < PortCount; ++port) {
        links_[port] = configureLink(
            "port" + std::to_string(port),
            new SST::Event::Handler<
                WormholeRouter,
                &WormholeRouter::handlePortEvent,
                int>(this, static_cast<int>(port)));
        outputCredits_[port] = inputBufferFlits_;
        roundRobin_[port] = port;
        const std::string name = portName(port);
        flitsForwarded_[port] =
            registerStatistic<std::uint64_t>("flits_forwarded", name);
        packetsForwarded_[port] =
            registerStatistic<std::uint64_t>("packets_forwarded", name);
        inputBufferFullCycles_[port] = registerStatistic<std::uint64_t>(
            "input_buffer_full_cycles", name);
        switchArbitrationStallCycles_[port] =
            registerStatistic<std::uint64_t>(
                "switch_arbitration_stall_cycles", name);
        outputCreditStallCycles_[port] = registerStatistic<std::uint64_t>(
            "output_credit_stall_cycles", name);
        outputLinkBusyCycles_[port] = registerStatistic<std::uint64_t>(
            "output_link_busy_cycles", name);
        inputBufferOccupancy_[port] = registerStatistic<std::uint64_t>(
            "input_buffer_occupancy", name);
    }
}

WormholeRouter::~WormholeRouter()
{
    for (auto& input : inputs_) {
        while (!input.empty()) {
            delete input.front().event;
            input.pop_front();
        }
    }
}

void WormholeRouter::init(unsigned)
{
    for (auto* link : links_) {
        if (link == nullptr) {
            continue;
        }
        while (SST::Event* raw = link->recvUntimedData()) {
            auto* flit = dynamic_cast<WormholeFlitEvent*>(raw);
            if (flit == nullptr) {
                delete raw;
                output_.fatal(
                    CALL_INFO,
                    -1,
                    "router %u received invalid untimed network data\n",
                    static_cast<unsigned>(routerId_));
            }
            forwardUntimed(flit);
        }
    }
}

void WormholeRouter::handlePortEvent(SST::Event* event, int portValue)
{
    const auto port = static_cast<std::uint32_t>(portValue);
    if (auto* credit = dynamic_cast<WormholeCreditEvent*>(event)) {
        if (credit->credits() >
            inputBufferFlits_ - outputCredits_[port]) {
            delete credit;
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u output %u received excess credits\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(port));
        }
        outputCredits_[port] += credit->credits();
        delete credit;
        ensureClock();
        return;
    }

    auto* flit = dynamic_cast<WormholeFlitEvent*>(event);
    if (flit == nullptr) {
        delete event;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u input %u received an invalid event\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(port));
    }
    if (inputs_[port].size() >= inputBufferFlits_) {
        delete flit;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u input %u overflowed its %u-flit buffer\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(port),
            static_cast<unsigned>(inputBufferFlits_));
    }
    const std::uint64_t cycle =
        clockFactor_ == 0 ? 0 : getCurrentSimCycle() / clockFactor_;
    inputs_[port].push_back(BufferedFlit{
        flit,
        cycle + (flit->head() ? pipelineCycles_ : 0),
    });
    ensureClock();
}

bool WormholeRouter::clock(SST::Cycle_t cycle)
{
    for (std::uint32_t input = 0; input < PortCount; ++input) {
        inputBufferOccupancy_[input]->addData(inputs_[input].size());
        if (inputs_[input].size() == inputBufferFlits_) {
            inputBufferFullCycles_[input]->addData(1);
        }
    }

    std::array<std::optional<std::uint32_t>, PortCount> selected{};
    std::array<bool, PortCount> requested{};
    for (std::uint32_t output = 0; output < PortCount; ++output) {
        if (outputOwners_[output].has_value()) {
            const std::uint32_t input = *outputOwners_[output];
            if (!inputs_[input].empty()) {
                requested[output] = true;
                selected[output] = input;
            }
            for (std::uint32_t blocked = 0;
                 blocked < PortCount;
                 ++blocked) {
                if (blocked == input || inputs_[blocked].empty() ||
                    inputRoutes_[blocked].has_value()) {
                    continue;
                }
                const BufferedFlit& candidate =
                    inputs_[blocked].front();
                if (candidate.event->head() &&
                    candidate.readyCycle <= cycle &&
                    route(candidate.event->destination()) == output) {
                    switchArbitrationStallCycles_[output]->addData(1);
                }
            }
            continue;
        }

        for (std::uint32_t offset = 1; offset <= PortCount; ++offset) {
            const std::uint32_t input =
                (roundRobin_[output] + offset) % PortCount;
            if (inputs_[input].empty() ||
                inputRoutes_[input].has_value()) {
                continue;
            }
            const BufferedFlit& buffered = inputs_[input].front();
            if (!buffered.event->head() ||
                buffered.readyCycle > cycle ||
                route(buffered.event->destination()) != output) {
                continue;
            }
            requested[output] = true;
            if (!selected[output].has_value()) {
                selected[output] = input;
            } else {
                switchArbitrationStallCycles_[output]->addData(1);
            }
        }
    }

    for (std::uint32_t output = 0; output < PortCount; ++output) {
        if (!requested[output] || !selected[output].has_value()) {
            continue;
        }
        if (links_[output] == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u selected disconnected output %u\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(output));
        }
        if (outputCredits_[output] == 0) {
            outputCreditStallCycles_[output]->addData(1);
            continue;
        }
        const std::uint32_t input = *selected[output];
        roundRobin_[output] = input;
        if (!outputOwners_[output].has_value()) {
            outputOwners_[output] = input;
            inputRoutes_[input] = output;
        }
        sendFromInput(input, output, flitsPerCycle_, cycle);
    }

    if (!hasWork()) {
        clockRegistered_ = false;
        return true;
    }
    return false;
}

void WormholeRouter::ensureClock()
{
    if (clockRegistered_) {
        return;
    }
    reregisterClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

bool WormholeRouter::hasWork() const noexcept
{
    return std::any_of(
        inputs_.begin(),
        inputs_.end(),
        [](const auto& input) { return !input.empty(); });
}

std::uint32_t WormholeRouter::route(
    std::uint32_t destination) const
{
    if (destination >= meshWidth_ * meshHeight_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u received invalid destination %u\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(destination));
    }
    if (destination == routerId_) {
        return Local;
    }
    const std::uint32_t x = routerId_ % meshWidth_;
    const std::uint32_t y = routerId_ / meshWidth_;
    const std::uint32_t destinationX = destination % meshWidth_;
    const std::uint32_t destinationY = destination / meshWidth_;
    if (destinationX > x) {
        return East;
    }
    if (destinationX < x) {
        return West;
    }
    return destinationY > y ? South : North;
}

void WormholeRouter::sendFromInput(
    std::uint32_t input,
    std::uint32_t output,
    std::uint32_t budget,
    std::uint64_t cycle)
{
    std::uint32_t sent = 0;
    while (sent < budget && outputCredits_[output] != 0 &&
           !inputs_[input].empty()) {
        BufferedFlit buffered = inputs_[input].front();
        if (buffered.readyCycle > cycle) {
            break;
        }
        inputs_[input].pop_front();
        WormholeFlitEvent* flit = buffered.event;
        const bool tail = flit->tail();
        links_[output]->send(flit);
        --outputCredits_[output];
        links_[input]->send(new WormholeCreditEvent());
        flitsForwarded_[output]->addData(1);
        ++sent;

        if (tail) {
            packetsForwarded_[output]->addData(1);
            outputOwners_[output].reset();
            inputRoutes_[input].reset();
            break;
        }
    }
    if (sent != 0) {
        outputLinkBusyCycles_[output]->addData(1);
    }
}

void WormholeRouter::forwardUntimed(WormholeFlitEvent* flit)
{
    const std::uint32_t output = route(flit->destination());
    if (links_[output] == nullptr) {
        delete flit;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u has no untimed route to destination\n",
            static_cast<unsigned>(routerId_));
    }
    links_[output]->sendUntimedData(flit);
}

std::string WormholeRouter::portName(std::uint32_t port) const
{
    static constexpr const char* names[PortCount] = {
        "east", "west", "south", "north", "local"};
    return names[port];
}

} // namespace Mittens
} // namespace SST
