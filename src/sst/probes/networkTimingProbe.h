#ifndef SST_MITTENS_NETWORK_TIMING_PROBE_H
#define SST_MITTENS_NETWORK_TIMING_PROBE_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

namespace SST {
namespace Mittens {

class NetworkTimingProbe final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        NetworkTimingProbe,
        "mittens",
        "networkTimingProbe",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "SST-only endpoint for controlled mesh timing validation",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        {"endpoint_id", "Expected Merlin endpoint identifier"},
        {"network_size", "Number of endpoints in the mesh"},
        {"destination", "Destination endpoint; negative disables transmit", "-1"},
        {"payload_words", "Number of 32-bit words in the test packet", "1"},
        {"send_cycle", "Probe-clock cycle at which a source first injects", "100"},
        {"receive_cycle", "Probe-clock cycle before which receives remain queued", "0"},
        {"expected_receives", "Packets this endpoint must receive", "0"},
        {"output_path", "Destination CSV written by a receiving endpoint", ""},
        {"clock", "Probe clock used for simultaneous injection and timeout", "1GHz"},
        {"link_clock", "Clock defining one physical mesh transfer cycle", "1GHz"},
        {"link_width_bits", "Physical mesh link width in bits per link cycle", "32"},
        {"tail_delivery", "The network delivers requests only after the tail flit arrives", "false"},
        {"timeout_cycles", "Fatal timeout measured in probe-clock cycles", "100000"},
        {"verbose", "Diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS()

    SST_ELI_DOCUMENT_STATISTICS()

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Merlin SimpleNetwork interface",
         "SST::Interfaces::SimpleNetwork"})

    NetworkTimingProbe(SST::ComponentId_t id, SST::Params& params);

    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void setup() override;
    void finish() override;

  private:
    struct Receipt {
        std::uint32_t source;
        std::uint32_t destination;
        std::uint32_t payloadWords;
        std::uint64_t injectionTick;
        std::uint64_t deliveryTick;
        std::uint64_t completionTick;
    };

    bool clock(SST::Cycle_t cycle);
    bool handleReceive(int virtualNetwork);
    void handleCompletion(SST::Event* event);
    bool sendPacket();
    void completeReadyReceipts();
    void signalCompletionIfReady();
    void writeReceipts();

    std::uint32_t endpointId_;
    std::uint32_t networkSize_;
    std::int64_t destination_;
    std::uint32_t payloadWords_;
    std::uint64_t sendCycle_;
    std::uint64_t receiveCycle_;
    std::uint32_t expectedReceives_;
    std::string outputPath_;
    std::uint32_t linkWidthBits_;
    bool tailDelivery_;
    std::uint64_t timeoutCycles_;
    SST::Output output_;
    SST::Interfaces::SimpleNetwork* network_ = nullptr;
    SST::TimeConverter linkClockTimeBase_;
    SST::Link* completionLink_ = nullptr;
    bool networkIdValidated_ = false;
    bool sent_ = false;
    bool receiveEnabled_ = false;
    bool completionSignaled_ = false;
    bool receiptsWritten_ = false;
    std::uint64_t nextCompletionAvailableTick_ = 0;
    std::deque<Receipt> pendingReceipts_;
    std::vector<Receipt> receipts_;
};

} // namespace Mittens
} // namespace SST

#endif
