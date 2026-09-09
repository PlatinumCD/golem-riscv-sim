#include "wormholeRouter.h"
#include <stdexcept>

#include <algorithm>

namespace SST {
namespace Mittens {

namespace {

class WormholeBurstReleaseEvent final : public SST::Event
{
  public:
    WormholeBurstReleaseEvent(
        std::uint32_t input,
        std::uint32_t output,
        std::uint64_t tailCycle) :
        input_(input),
        output_(output),
        tailCycle_(tailCycle)
    {
    }

    std::uint32_t input() const noexcept { return input_; }
    std::uint32_t output() const noexcept { return output_; }
    std::uint64_t tailCycle() const noexcept { return tailCycle_; }

  private:
    std::uint32_t input_ = 0;
    std::uint32_t output_ = 0;
    std::uint64_t tailCycle_ = 0;
};

} // namespace

WormholeRouter::WormholeRouter(
    SST::ComponentId_t id,
    SST::Params& params) :
    SST::Component(id),
    configuration_(RouterConfiguration::read(params)),
    routerId_(configuration_.id),
    meshWidth_(configuration_.mesh_width),
    meshHeight_(configuration_.mesh_height),
    inputBufferFlits_(
        configuration_.input_buffer_flits),
    pipelineCycles_(configuration_.pipeline_cycles),
    flitsPerCycle_(
        configuration_.link_width_bits / 32),
    packetBurstCoalescing_(
        configuration_.packet_burst_coalescing),
    txStreams_(configuration_.tx_streams),
    rxStreams_(configuration_.rx_streams),
    output_(
        "mittens-wormhole-router: ",
        configuration_.verbose,
        0,
        SST::Output::STDOUT)
{
    configuration_.validate(output_);
    configuration_.emit();
    loopbackInjectionOutput_ = params.find<int>("loopback_injection_output", -1);
    if (loopbackInjectionOutput_ < -1 || loopbackInjectionOutput_ > 3 ||
        (loopbackInjectionOutput_ == East && routerId_%meshWidth_+1 == meshWidth_) ||
        (loopbackInjectionOutput_ == West && routerId_%meshWidth_ == 0) ||
        (loopbackInjectionOutput_ == South && routerId_/meshWidth_+1 == meshHeight_) ||
        (loopbackInjectionOutput_ == North && routerId_/meshWidth_ == 0))
        output_.fatal(CALL_INFO, -1, "invalid loopback_injection_output at router %u\n", routerId_);
    // Explicit experimental destination routing; empty preserves ordinary XY.
    // Format: destination:cardinal-output, separated by commas (E=0,W=1,S=2,N=3).
    const auto overrides = params.find<std::string>("route_overrides", "");
    std::size_t cursor = 0;
    while (cursor < overrides.size()) {
        const auto end = overrides.find(',', cursor);
        const auto token = overrides.substr(cursor, end - cursor);
        const auto colon = token.find(':');
        try {
            std::size_t usedD=0, usedP=0;
            if (colon == std::string::npos) throw std::invalid_argument("separator");
            const auto dest = std::stoul(token.substr(0, colon), &usedD);
            const auto port = std::stoul(token.substr(colon+1), &usedP);
            if (usedD != colon || usedP != token.size()-colon-1 ||
                dest >= meshWidth_*meshHeight_ || dest == routerId_ || port >= 4 ||
                (port == East && routerId_%meshWidth_+1 == meshWidth_) ||
                (port == West && routerId_%meshWidth_ == 0) ||
                (port == South && routerId_/meshWidth_+1 == meshHeight_) ||
                (port == North && routerId_/meshWidth_ == 0) ||
                !routeOverrides_.emplace(dest,port).second)
                throw std::invalid_argument("invalid destination/output");
        } catch (const std::exception&) {
            output_.fatal(CALL_INFO, -1, "router %u invalid route_overrides: %s\n", routerId_, overrides.c_str());
        }
        if (end == std::string::npos) break;
        cursor = end+1;
        if (cursor == overrides.size()) output_.fatal(CALL_INFO,-1,"trailing route_overrides separator\n");
    }
    const auto tracePath = params.find<std::string>("flit_trace_path", "");
    const auto packetTracePath = params.find<std::string>("packet_trace_path", "");
    if (!packetTracePath.empty()) {
        packetTrace_.open(packetTracePath);
        if (!packetTrace_) output_.fatal(CALL_INFO,-1,"cannot open packet trace\n");
        packetTrace_ << "event,cycle,source,packet_id,input,output,head_ready_cycle\n";
    }
    if (!tracePath.empty()) {
        if (packetBurstCoalescing_) {
            output_.fatal(CALL_INFO, -1, "flit tracing requires packet_burst_coalescing=false\n");
        }
        flitTrace_.open(tracePath);
        if (!flitTrace_) output_.fatal(CALL_INFO, -1, "cannot open flit trace %s\n", tracePath.c_str());
        flitTrace_ << "time_ps,router,input,output,tail\n";
    }
    clockTimeBase_ = getTimeConverter(
        configuration_.clock);
    clockFactor_ = clockTimeBase_.getFactor();
    clockHandler_ = new SST::Clock::Handler<
        WormholeRouter,
        &WormholeRouter::clock>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
    burstReleaseLink_ = configureSelfLink(
        "packet-burst-release",
        clockTimeBase_,
        new SST::Event::Handler<
            WormholeRouter,
            &WormholeRouter::handleBurstRelease>(this));
    if (burstReleaseLink_ == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u could not configure packet-burst release\n",
            static_cast<unsigned>(routerId_));
    }
    for (std::uint32_t port = 0; port < 4 + std::max(txStreams_, rxStreams_); ++port) {
        links_[port] = configureLink(
            "port" + std::to_string(port),
            new SST::Event::Handler<
                WormholeRouter,
                &WormholeRouter::handlePortEvent,
                int>(this, static_cast<int>(port)));
    }
    for (std::uint32_t port = 0; port < OutputCount; ++port) {
        outputCredits_[port] = inputBufferFlits_;
        roundRobin_[port] = port;
        const std::string name = portName(port);
        flitsForwarded_[port] =
            registerStatistic<std::uint64_t>("flits_forwarded", name);
        packetsForwarded_[port] =
            registerStatistic<std::uint64_t>("packets_forwarded", name);
        switchArbitrationStallCycles_[port] =
            registerStatistic<std::uint64_t>(
                "switch_arbitration_stall_cycles", name);
        outputCreditStallCycles_[port] = registerStatistic<std::uint64_t>(
            "output_credit_stall_cycles", name);
        outputLinkBusyCycles_[port] = registerStatistic<std::uint64_t>(
            "output_link_busy_cycles", name);
    }
    for (std::uint32_t input = 0; input < 4 + txStreams_; ++input) {
        const std::string name = portName(input);
        inputBufferFullCycles_[input] = registerStatistic<std::uint64_t>(
            "input_buffer_full_cycles", name);
        inputBufferOccupancy_[input] = registerStatistic<std::uint64_t>(
            "input_buffer_occupancy", name);
    }
    cardinalOutputsActive_ = registerStatistic<std::uint64_t>(
        "cardinal_outputs_active");
}

WormholeRouter::~WormholeRouter()
{
    for (auto& input : inputs_) {
        while (!input.empty()) {
            delete input.front().event;
            input.pop_front();
        }
    }
    for (auto& bursts : incomingPacketBursts_) {
        for (auto& burst : bursts) {
            delete burst.request;
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
    const std::uint64_t cycle = clockFactor_ == 0
        ? 0
        : getCurrentSimCycle() / clockFactor_;
    if (auto* burst = dynamic_cast<WormholeCreditBurstEvent*>(event)) {
        if (burst->credits() == 0 || burst->creditsPerCycle() == 0) {
            delete burst;
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u output %u received an invalid credit burst\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(port));
        }
        pendingCreditBursts_[port].push_back(PendingCreditBurst{
            cycle,
            burst->credits(),
            burst->creditsPerCycle(),
        });
        delete burst;
        accrueOutputCredits(port, cycle);
        ensureClock();
        return;
    }
    if (auto* credit = dynamic_cast<WormholeCreditEvent*>(event)) {
        accrueOutputCredits(port, cycle);
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

    if (auto* packet = dynamic_cast<WormholePacketBurstEvent*>(event)) {
        if (packet->count() == 0 ||
            packet->flitsPerCycle() != flitsPerCycle_ ||
            packet->count() >
                inputBufferFlits_ - inputReservedFlits_[port]) {
            delete packet;
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u input %u received an invalid packet burst\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(port));
        }
        auto* request = packet->takeRequest();
        if (request == nullptr) {
            delete packet;
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u input %u received an empty packet burst\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(port));
        }
        incomingPacketBursts_[port].push_back(IncomingPacketBurst{
            packet->packetId(),
            packet->source(),
            packet->destination(),
            packet->virtualNetwork(),
            packet->count(),
            packet->flitsPerCycle(),
            0,
            cycle,
            packet->injectionTick(),
            request,
        });
        inputReservedFlits_[port] += packet->count();
        delete packet;
        materializeInputBursts(port, cycle + 1);
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
    if (inputReservedFlits_[port] >= inputBufferFlits_) {
        delete flit;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u input %u overflowed its %u-flit buffer\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(port),
            static_cast<unsigned>(inputBufferFlits_));
    }
    inputs_[port].push_back(BufferedFlit{
        flit,
        cycle + (flit->head() ? pipelineCycles_ : 0),
    });
    ++inputReservedFlits_[port];
    ensureClock();
}

void WormholeRouter::handleBurstRelease(SST::Event* event)
{
    auto* release = dynamic_cast<WormholeBurstReleaseEvent*>(event);
    if (release == nullptr || release->input() >= InputCount ||
        release->output() >= OutputCount) {
        delete event;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u received an invalid packet-burst release\n",
            static_cast<unsigned>(routerId_));
    }
    const std::uint32_t input = release->input();
    const std::uint32_t output = release->output();
    const bool current = burstOutputActive_[output] &&
        burstTailCycle_[output] == release->tailCycle() &&
        outputOwners_[output] == input && inputRoutes_[input] == output;
    delete release;
    if (!current) {
        return;
    }
    burstOutputActive_[output] = false;
    outputOwners_[output].reset();
    inputRoutes_[input].reset();
    if (hasWork()) {
        ensureClock();
    }
}

bool WormholeRouter::clock(SST::Cycle_t cycle)
{
    std::array<bool, 4> cardinalActive{};
    for (std::uint32_t output = 0; output < 4; ++output) {
        cardinalActive[output] = burstOutputActive_[output];
    }
    for (std::uint32_t port = 0; port < OutputCount; ++port) {
        materializeInputBursts(port, cycle);
        accrueOutputCredits(port, cycle);
    }
    for (std::uint32_t input = 0; input < 4 + txStreams_; ++input) {
        inputBufferOccupancy_[input]->addData(inputs_[input].size());
        if (inputs_[input].size() == inputBufferFlits_) {
            inputBufferFullCycles_[input]->addData(1);
        }
    }

    const SwitchDecision decision = selectInputs(cycle);
    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        if (decision.arbitrationStalls[output] != 0) {
            switchArbitrationStallCycles_[output]->addDataNTimes(
                decision.arbitrationStalls[output],
                UINT64_C(1));
        }
    }

    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        if (!decision.requested[output] ||
            !decision.selected[output].has_value()) {
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
        const std::uint32_t input = *decision.selected[output];
        roundRobin_[output] = input;
        if (!outputOwners_[output].has_value()) {
            if (packetTrace_.is_open()) {
                const auto& head = inputs_[input].front();
                packetTrace_ << "grant," << cycle << ',' << head.event->source() << ','
                             << head.event->packetId() << ',' << input << ',' << output << ','
                             << head.readyCycle << '\n';
            }
            outputOwners_[output] = input;
            inputRoutes_[input] = output;
        }
        if (trySendPacketBurst(
                input,
                output,
                decision.arbitrationStalls[output],
                cycle)) {
            if (output < 4) {
                cardinalActive[output] = true;
            }
            continue;
        }
        if (sendFromInput(input, output, flitsPerCycle_, cycle) &&
            output < 4) {
            cardinalActive[output] = true;
        }
    }

    cardinalOutputsActive_->addData(static_cast<std::uint64_t>(
        std::count(cardinalActive.begin(), cardinalActive.end(), true)));

    if (!hasWork()) {
        clockRegistered_ = false;
        return true;
    }
    if (beginCreditStallSleep(cycle)) {
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
    const SST::Cycle_t nextCycle =
        reregisterClock(clockTimeBase_, clockHandler_);
    accountCreditStallSleep(nextCycle);
    clockRegistered_ = true;
}

void WormholeRouter::materializeInputBursts(
    std::uint32_t input,
    std::uint64_t cycle)
{
    for (auto& burst : incomingPacketBursts_[input]) {
        if (cycle <= burst.headArrivalCycle ||
            burst.nextIndex >= burst.count) {
            continue;
        }
        const std::uint64_t elapsed = cycle - burst.headArrivalCycle;
        const std::uint64_t due = std::min<std::uint64_t>(
            burst.count,
            elapsed * burst.flitsPerCycle);
        while (burst.nextIndex < due) {
            const std::uint32_t index = burst.nextIndex++;
            auto* request = index == 0 ? burst.request : nullptr;
            if (index == 0) {
                burst.request = nullptr;
            }
            inputs_[input].push_back(BufferedFlit{
                new WormholeFlitEvent(
                    burst.packetId,
                    burst.source,
                    burst.destination,
                    burst.virtualNetwork,
                    index,
                    burst.count,
                    burst.injectionTick,
                    request),
                burst.headArrivalCycle +
                    (index == 0
                        ? pipelineCycles_
                        : index / burst.flitsPerCycle),
            });
        }
    }
    while (!incomingPacketBursts_[input].empty() &&
           incomingPacketBursts_[input].front().nextIndex ==
               incomingPacketBursts_[input].front().count) {
        if (incomingPacketBursts_[input].front().request != nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u input %u failed to materialize a packet head\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(input));
        }
        incomingPacketBursts_[input].pop_front();
    }
}

void WormholeRouter::accrueOutputCredits(
    std::uint32_t output,
    std::uint64_t cycle)
{
    auto& bursts = pendingCreditBursts_[output];
    for (auto iterator = bursts.begin(); iterator != bursts.end();) {
        if (cycle < iterator->firstCycle) {
            ++iterator;
            continue;
        }
        const std::uint64_t elapsed = cycle - iterator->firstCycle + 1;
        const std::uint64_t available = std::min<std::uint64_t>(
            iterator->credits,
            elapsed * iterator->creditsPerCycle);
        if (available > inputBufferFlits_ - outputCredits_[output]) {
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u output %u credit burst exceeded capacity\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(output));
        }
        outputCredits_[output] += static_cast<std::uint32_t>(available);
        iterator->credits -= static_cast<std::uint32_t>(available);
        if (iterator->credits == 0) {
            iterator = bursts.erase(iterator);
            continue;
        }
        const std::uint64_t consumedCycles =
            (available + iterator->creditsPerCycle - 1) /
            iterator->creditsPerCycle;
        iterator->firstCycle += consumedCycles;
        ++iterator;
    }
}

bool WormholeRouter::trySendPacketBurst(
    std::uint32_t input,
    std::uint32_t output,
    std::uint32_t arbitrationStalls,
    std::uint64_t cycle)
{
    if (!packetBurstCoalescing_ || arbitrationStalls != 0 ||
        inputs_[input].empty()) {
        return false;
    }
    WormholeFlitEvent* head = inputs_[input].front().event;
    if (!head->head() || head->count() == 0 ||
        outputCredits_[output] < head->count()) {
        return false;
    }
    const std::uint64_t packetId = head->packetId();
    const std::uint32_t packetSource = head->source();
    const std::uint32_t count = head->count();
    const std::uint32_t materialized = static_cast<std::uint32_t>(
        std::count_if(
            inputs_[input].begin(),
            inputs_[input].end(),
            [packetId, packetSource](const BufferedFlit& buffered) {
                return buffered.event->packetId() == packetId &&
                    buffered.event->source() == packetSource;
            }));
    const IncomingPacketBurst* pending = nullptr;
    for (const auto& burst : incomingPacketBursts_[input]) {
        if (burst.packetId == packetId && burst.source == packetSource) {
            pending = &burst;
            break;
        }
    }
    if (pending == nullptr && materialized != count) {
        return false;
    }
    if (pending != nullptr &&
        materialized + (pending->count - pending->nextIndex) != count) {
        return false;
    }

    auto* request = head->takeRequest();
    if (request == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u input %u packet burst has no request\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(input));
    }
    auto* packet = new WormholePacketBurstEvent(
        packetId,
        head->source(),
        head->destination(),
        head->virtualNetwork(),
        count,
        flitsPerCycle_,
        head->injectionTick(),
        request);

    for (auto iterator = inputs_[input].begin();
         iterator != inputs_[input].end();) {
        if (iterator->event->packetId() != packetId ||
            iterator->event->source() != packetSource) {
            ++iterator;
            continue;
        }
        delete iterator->event;
        iterator = inputs_[input].erase(iterator);
    }
    for (auto iterator = incomingPacketBursts_[input].begin();
         iterator != incomingPacketBursts_[input].end(); ++iterator) {
        if (iterator->packetId != packetId ||
            iterator->source != packetSource) {
            continue;
        }
        if (iterator->request != nullptr) {
            delete packet;
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u input %u packet burst retained two requests\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(input));
        }
        incomingPacketBursts_[input].erase(iterator);
        break;
    }
    if (count > inputReservedFlits_[input]) {
        delete packet;
        output_.fatal(
            CALL_INFO,
            -1,
            "router %u input %u packet reservation underflowed\n",
            static_cast<unsigned>(routerId_),
            static_cast<unsigned>(input));
    }
    inputReservedFlits_[input] -= count;
    outputCredits_[output] -= count;
    links_[output]->send(packet);
    links_[input]->send(new WormholeCreditBurstEvent(
        count,
        flitsPerCycle_));
    flitsForwarded_[output]->addDataNTimes(count, UINT64_C(1));
    packetsForwarded_[output]->addData(1);
    const std::uint64_t cycles = serializationCycles(count);
    outputLinkBusyCycles_[output]->addDataNTimes(cycles, UINT64_C(1));
    const std::uint64_t tailCycle = cycle + cycles - 1;
    burstOutputActive_[output] = true;
    burstTailCycle_[output] = tailCycle;
    burstReleaseLink_->send(
        cycles - 1,
        new WormholeBurstReleaseEvent(input, output, tailCycle));
    return true;
}

std::uint64_t WormholeRouter::serializationCycles(
    std::uint32_t flits) const
{
    return flits / flitsPerCycle_ +
        (flits % flitsPerCycle_ != 0 ? 1 : 0);
}

WormholeRouter::SwitchDecision WormholeRouter::selectInputs(
    std::uint64_t cycle) const
{
    SwitchDecision decision;
    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        if (outputOwners_[output].has_value()) {
            const std::uint32_t input = *outputOwners_[output];
            if (!inputs_[input].empty()) {
                decision.requested[output] = true;
                decision.selected[output] = input;
            }
            for (std::uint32_t blocked = 0;
                 blocked < 4 + txStreams_;
                 ++blocked) {
                if (blocked == input || inputs_[blocked].empty() ||
                    inputRoutes_[blocked].has_value()) {
                    continue;
                }
                const BufferedFlit& candidate =
                    inputs_[blocked].front();
                if (candidate.event->head() &&
                    candidate.readyCycle <= cycle &&
                    (route(candidate.event->destination(), blocked) == output ||
                     (route(candidate.event->destination(), blocked) == Local && output >= Local && output < Local + rxStreams_))) {
                    ++decision.arbitrationStalls[output];
                }
            }
            continue;
        }

        for (std::uint32_t offset = 1; offset <= 4 + txStreams_; ++offset) {
            const std::uint32_t input =
                (roundRobin_[output] + offset) % (4 + txStreams_);
            if (inputs_[input].empty() ||
                inputRoutes_[input].has_value()) {
                continue;
            }
            const BufferedFlit& buffered = inputs_[input].front();
            if (!buffered.event->head() ||
                buffered.readyCycle > cycle ||
                !(route(buffered.event->destination(), input) == output ||
                  (route(buffered.event->destination(), input) == Local && output >= Local && output < Local + rxStreams_))) {
                continue;
            }
            // One input may win only one output in this cycle. Packet ownership
            // then holds that route through the tail, preserving source order.
            bool alreadySelected = false;
            for (std::uint32_t prior = 0; prior < output; ++prior)
                alreadySelected |= decision.selected[prior] == input;
            if (alreadySelected) continue;
            decision.requested[output] = true;
            if (!decision.selected[output].has_value()) {
                decision.selected[output] = input;
            } else {
                ++decision.arbitrationStalls[output];
            }
        }
    }
    return decision;
}

bool WormholeRouter::beginCreditStallSleep(std::uint64_t cycle)
{
    const std::uint64_t nextCycle = cycle + 1;
    for (const auto& input : inputs_) {
        if (!input.empty() && input.front().readyCycle > nextCycle) {
            return false;
        }
    }

    const SwitchDecision decision = selectInputs(nextCycle);
    bool requested = false;
    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        if (!decision.requested[output] ||
            !decision.selected[output].has_value()) {
            continue;
        }
        requested = true;
        if (outputCredits_[output] != 0) {
            return false;
        }
    }
    if (!requested) {
        return false;
    }

    for (std::uint32_t input = 0; input < 4 + txStreams_; ++input) {
        sleepingOccupancy_[input] = inputs_[input].size();
    }
    sleepingArbitrationStalls_ = decision.arbitrationStalls;
    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        sleepingCreditStalls_[output] =
            decision.requested[output] &&
            decision.selected[output].has_value() &&
            outputCredits_[output] == 0;
    }
    clockStoppedAfterCycle_ = cycle;
    creditStallSleeping_ = true;
    return true;
}

void WormholeRouter::accountCreditStallSleep(SST::Cycle_t nextCycle)
{
    if (!creditStallSleeping_) {
        return;
    }
    const std::uint64_t firstSkippedCycle =
        clockStoppedAfterCycle_ + 1;
    const std::uint64_t skippedCycles = nextCycle > firstSkippedCycle
        ? nextCycle - firstSkippedCycle
        : 0;
    if (skippedCycles == 0) {
        creditStallSleeping_ = false;
        return;
    }
    for (std::uint32_t input = 0; input < 4 + txStreams_; ++input) {
        inputBufferOccupancy_[input]->addDataNTimes(
            skippedCycles,
            sleepingOccupancy_[input]);
        if (sleepingOccupancy_[input] == inputBufferFlits_) {
            inputBufferFullCycles_[input]->addDataNTimes(
                skippedCycles,
                UINT64_C(1));
        }
    }
    for (std::uint32_t output = 0; output < OutputCount; ++output) {
        if (sleepingArbitrationStalls_[output] != 0) {
            switchArbitrationStallCycles_[output]->addDataNTimes(
                skippedCycles * sleepingArbitrationStalls_[output],
                UINT64_C(1));
        }
        if (sleepingCreditStalls_[output]) {
            outputCreditStallCycles_[output]->addDataNTimes(
                skippedCycles,
                UINT64_C(1));
        }
    }
    creditStallSleeping_ = false;
}

bool WormholeRouter::hasWork() const noexcept
{
    return std::any_of(
        inputs_.begin(),
        inputs_.end(),
        [](const auto& input) { return !input.empty(); });
}

std::uint32_t WormholeRouter::route(
    std::uint32_t destination, std::uint32_t input) const
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
        if (input >= Local && loopbackInjectionOutput_ >= 0)
            return static_cast<std::uint32_t>(loopbackInjectionOutput_);
        return Local;
    }
    const auto override = routeOverrides_.find(destination);
    if (override != routeOverrides_.end()) return override->second;
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

bool WormholeRouter::sendFromInput(
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
        const auto traceSource = flit->source();
        const auto tracePacket = flit->packetId();
        if (inputReservedFlits_[input] == 0) {
            output_.fatal(
                CALL_INFO,
                -1,
                "router %u input %u reservation underflowed\n",
                static_cast<unsigned>(routerId_),
                static_cast<unsigned>(input));
        }
        if (flitTrace_.is_open()) {
            flitTrace_ << getCurrentSimTimeNano() * 1000 << ',' << routerId_ << ','
                       << input << ',' << output << ',' << tail << '\n';
        }
        links_[output]->send(flit);
        --outputCredits_[output];
        --inputReservedFlits_[input];
        links_[input]->send(new WormholeCreditEvent());
        flitsForwarded_[output]->addData(1);
        ++sent;

        if (tail) {
            if (packetTrace_.is_open())
                packetTrace_ << "tail," << cycle << ',' << traceSource << ','
                             << tracePacket << ',' << input << ',' << output << ",\n";
            packetsForwarded_[output]->addData(1);
            outputOwners_[output].reset();
            inputRoutes_[input].reset();
            break;
        }
    }
    if (sent != 0) {
        outputLinkBusyCycles_[output]->addData(1);
    }
    return sent != 0;
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
    static constexpr const char* names[InputCount] = {
        "east", "west", "south", "north",
        "local0", "local1", "local2", "local3"};
    return names[port];
}

} // namespace Mittens
} // namespace SST
