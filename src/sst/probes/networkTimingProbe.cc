#include "networkTimingProbe.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "../network/packetEvent.h"

namespace SST {
namespace Mittens {

namespace {

constexpr int kVirtualNetwork = 0;
constexpr std::uint64_t kWordBits = 32;

std::uint64_t divideRoundUp(
    std::uint64_t value,
    std::uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

} // namespace

NetworkTimingProbe::NetworkTimingProbe(
    SST::ComponentId_t id,
    SST::Params& params) :
    SST::Component(id),
    endpointId_(params.find<std::uint32_t>("endpoint_id")),
    networkSize_(params.find<std::uint32_t>("network_size")),
    destination_(params.find<std::int64_t>("destination", -1)),
    payloadWords_(params.find<std::uint32_t>("payload_words", 1)),
    sendCycle_(params.find<std::uint64_t>("send_cycle", 100)),
    receiveCycle_(params.find<std::uint64_t>("receive_cycle", 0)),
    expectedReceives_(
        params.find<std::uint32_t>("expected_receives", 0)),
    outputPath_(params.find<std::string>("output_path", "")),
    linkWidthBits_(
        params.find<std::uint32_t>("link_width_bits", 32)),
    tailDelivery_(params.find<bool>("tail_delivery", false)),
    timeoutCycles_(
        params.find<std::uint64_t>("timeout_cycles", 100000)),
    output_(
        "mittens-network-probe: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT)
{
    if (networkSize_ == 0 || endpointId_ >= networkSize_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u is outside network size %u\n",
            static_cast<unsigned>(endpointId_),
            static_cast<unsigned>(networkSize_));
    }
    if (destination_ >= static_cast<std::int64_t>(networkSize_)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u has invalid destination %lld\n",
            static_cast<unsigned>(endpointId_),
            static_cast<long long>(destination_));
    }
    if (payloadWords_ == 0 ||
        payloadWords_ >
            std::numeric_limits<std::size_t>::max() / kWordBits) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u has invalid payload size %u\n",
            static_cast<unsigned>(endpointId_),
            static_cast<unsigned>(payloadWords_));
    }
    if (linkWidthBits_ == 0 || linkWidthBits_ % kWordBits != 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u requires link_width_bits to be a positive "
            "multiple of 32\n",
            static_cast<unsigned>(endpointId_));
    }
    if (timeoutCycles_ <= sendCycle_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u timeout must follow send cycle\n",
            static_cast<unsigned>(endpointId_));
    }
    if (expectedReceives_ != 0 && outputPath_.empty()) {
        output_.fatal(
            CALL_INFO,
            -1,
            "receiving endpoint %u requires output_path\n",
            static_cast<unsigned>(endpointId_));
    }

    network_ = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
        "networkIF",
        SST::ComponentInfo::SHARE_NONE,
        1);
    if (network_ == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u requires networkIF\n",
            static_cast<unsigned>(endpointId_));
    }
    network_->setNotifyOnReceive(
        new SST::Interfaces::SimpleNetwork::Handler<
            NetworkTimingProbe,
            &NetworkTimingProbe::handleReceive>(this));
    receiveEnabled_ = receiveCycle_ == 0;

    linkClockTimeBase_ = getTimeConverter(
        params.find<std::string>("link_clock", "1GHz"));
    completionLink_ = configureSelfLink(
        "packet-completion",
        linkClockTimeBase_,
        new SST::Event::Handler<
            NetworkTimingProbe,
            &NetworkTimingProbe::handleCompletion>(this));
    if (completionLink_ == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u could not configure packet completion link\n",
            static_cast<unsigned>(endpointId_));
    }

    registerClock(
        params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler<
            NetworkTimingProbe,
            &NetworkTimingProbe::clock>(this));
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void NetworkTimingProbe::init(unsigned phase)
{
    network_->init(phase);
    if (network_->isNetworkInitialized() && !networkIdValidated_) {
        const auto actual = network_->getEndpointID();
        if (actual != static_cast<decltype(actual)>(endpointId_)) {
            output_.fatal(
                CALL_INFO,
                -1,
                "configured endpoint %u received Merlin ID %lld\n",
                static_cast<unsigned>(endpointId_),
                static_cast<long long>(actual));
        }
        networkIdValidated_ = true;
    }
}

void NetworkTimingProbe::complete(unsigned phase)
{
    network_->complete(phase);
}

void NetworkTimingProbe::setup()
{
    network_->setup();
    if (!networkIdValidated_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u did not receive a Merlin network ID\n",
            static_cast<unsigned>(endpointId_));
    }
    signalCompletionIfReady();
}

void NetworkTimingProbe::finish()
{
    if (expectedReceives_ != 0 && receipts_.size() != expectedReceives_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u received %zu of %u expected packets\n",
            static_cast<unsigned>(endpointId_),
            receipts_.size(),
            static_cast<unsigned>(expectedReceives_));
    }
    writeReceipts();
    network_->finish();
}

bool NetworkTimingProbe::clock(SST::Cycle_t cycle)
{
    if (cycle > timeoutCycles_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "endpoint %u timed out at probe cycle %llu\n",
            static_cast<unsigned>(endpointId_),
            static_cast<unsigned long long>(cycle));
    }
    if (destination_ >= 0 && !sent_ && cycle >= sendCycle_) {
        (void)sendPacket();
    }
    if (!receiveEnabled_ && cycle >= receiveCycle_) {
        receiveEnabled_ = true;
        (void)handleReceive(kVirtualNetwork);
    }
    signalCompletionIfReady();
    return completionSignaled_;
}

bool NetworkTimingProbe::sendPacket()
{
    const std::size_t packetBits =
        static_cast<std::size_t>(payloadWords_) * kWordBits;
    if (!network_->spaceToSend(kVirtualNetwork, packetBits)) {
        return false;
    }

    const std::uint64_t injectionTick = getCurrentSimCycle();
    std::vector<std::uint32_t> payload(
        payloadWords_,
        UINT32_C(0xa5000000) | endpointId_);
    PacketEvent::Metadata metadata;
    metadata.packetId = endpointId_;
    metadata.injectionTick = injectionTick;
    metadata.payloadWords = payloadWords_;

    auto* request = new SST::Interfaces::SimpleNetwork::Request(
        destination_,
        endpointId_,
        packetBits,
        true,
        true,
        new PacketEvent(std::move(payload), metadata));
    if (!network_->send(request, kVirtualNetwork)) {
        delete request;
        return false;
    }
    sent_ = true;
    output_.verbose(
        CALL_INFO,
        1,
        0,
        "endpoint %u injected %u words to %lld at tick %llu\n",
        static_cast<unsigned>(endpointId_),
        static_cast<unsigned>(payloadWords_),
        static_cast<long long>(destination_),
        static_cast<unsigned long long>(injectionTick));
    return true;
}

bool NetworkTimingProbe::handleReceive(int virtualNetwork)
{
    if (!receiveEnabled_) {
        return true;
    }
    while (network_->requestToReceive(virtualNetwork)) {
        SST::Interfaces::SimpleNetwork::Request* request =
            network_->recv(virtualNetwork);
        if (request == nullptr) {
            break;
        }

        SST::Event* rawPayload = request->takePayload();
        auto* packet = dynamic_cast<PacketEvent*>(rawPayload);
        if (packet == nullptr) {
            delete rawPayload;
            delete request;
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received an invalid timing payload\n",
                static_cast<unsigned>(endpointId_));
        }

        const std::uint32_t source =
            static_cast<std::uint32_t>(request->src);
        const auto metadata = packet->metadata();
        const std::size_t actualWords = packet->payloads().size();
        delete packet;
        delete request;

        if (actualWords != metadata.payloadWords ||
            actualWords != payloadWords_ ||
            source >= networkSize_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received malformed packet from %u\n",
                static_cast<unsigned>(endpointId_),
                static_cast<unsigned>(source));
        }
        if (std::any_of(
                receipts_.begin(),
                receipts_.end(),
                [source](const Receipt& receipt) {
                    return receipt.source == source;
                })) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received duplicate source %u\n",
                static_cast<unsigned>(endpointId_),
                static_cast<unsigned>(source));
        }

        const std::uint64_t headArrivalTick = getCurrentSimCycle();
        const std::uint64_t transferCycles = tailDelivery_
            ? 0
            : divideRoundUp(
                  static_cast<std::uint64_t>(actualWords) * kWordBits,
                  linkWidthBits_);
        const std::uint64_t linkPeriod =
            linkClockTimeBase_.getFactor();
        if (transferCycles >
            std::numeric_limits<std::uint64_t>::max() /
                linkPeriod) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u packet completion time overflowed\n",
                static_cast<unsigned>(endpointId_));
        }
        const std::uint64_t transferTicks =
            transferCycles * linkPeriod;
        const std::uint64_t startTick = tailDelivery_
            ? headArrivalTick
            : std::max(
                  headArrivalTick,
                  nextCompletionAvailableTick_);
        if (startTick >
            std::numeric_limits<std::uint64_t>::max() -
                transferTicks) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u packet serialization overflowed\n",
                static_cast<unsigned>(endpointId_));
        }
        nextCompletionAvailableTick_ = startTick + transferTicks;
        const std::uint64_t completionTick = tailDelivery_
            ? headArrivalTick
            : nextCompletionAvailableTick_ - linkPeriod;
        const std::uint64_t completionDelayCycles =
            divideRoundUp(
                completionTick - headArrivalTick,
                linkPeriod);
        pendingReceipts_.push_back(Receipt{
            source,
            endpointId_,
            static_cast<std::uint32_t>(actualWords),
            metadata.injectionTick,
            headArrivalTick,
            completionTick,
        });
        if (receipts_.size() + pendingReceipts_.size() >
            expectedReceives_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "endpoint %u received more than %u packets\n",
                static_cast<unsigned>(endpointId_),
                static_cast<unsigned>(expectedReceives_));
        }
        if (completionDelayCycles == 0) {
            completeReadyReceipts();
        } else {
            completionLink_->send(
                completionDelayCycles,
                new SST::Event());
        }
    }
    signalCompletionIfReady();
    return !completionSignaled_;
}

void NetworkTimingProbe::handleCompletion(SST::Event* event)
{
    delete event;
    completeReadyReceipts();
    signalCompletionIfReady();
}

void NetworkTimingProbe::completeReadyReceipts()
{
    const std::uint64_t now = getCurrentSimCycle();
    auto receipt = pendingReceipts_.begin();
    while (receipt != pendingReceipts_.end()) {
        if (receipt->completionTick > now) {
            ++receipt;
            continue;
        }
        receipts_.push_back(*receipt);
        receipt = pendingReceipts_.erase(receipt);
    }
}

void NetworkTimingProbe::signalCompletionIfReady()
{
    if (completionSignaled_) {
        return;
    }
    const bool transmitComplete = destination_ < 0 || sent_;
    const bool receiveComplete =
        expectedReceives_ == 0 ||
        receipts_.size() == expectedReceives_;
    if (!transmitComplete || !receiveComplete) {
        return;
    }
    if (expectedReceives_ != 0) {
        writeReceipts();
    }
    completionSignaled_ = true;
    primaryComponentOKToEndSim();
}

void NetworkTimingProbe::writeReceipts()
{
    if (receiptsWritten_ || expectedReceives_ == 0) {
        return;
    }
    std::sort(
        receipts_.begin(),
        receipts_.end(),
        [](const Receipt& left, const Receipt& right) {
            return left.source < right.source;
        });
    std::ofstream stream(outputPath_, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error(
            "could not open network timing output: " + outputPath_);
    }
    stream
        << "source,destination,payload_words,injection_tick,"
           "head_arrival_tick,completion_tick,latency_ticks\n";
    for (const Receipt& receipt : receipts_) {
        stream
            << receipt.source << ','
            << receipt.destination << ','
            << receipt.payloadWords << ','
            << receipt.injectionTick << ','
            << receipt.headArrivalTick << ','
            << receipt.completionTick << ','
            << (receipt.completionTick - receipt.injectionTick)
            << '\n';
    }
    stream.flush();
    if (!stream) {
        throw std::runtime_error(
            "could not write network timing output: " + outputPath_);
    }
    receiptsWritten_ = true;
}

} // namespace Mittens
} // namespace SST
