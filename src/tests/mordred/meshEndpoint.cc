#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/unitAlgebra.h>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace TileComponents {

// Metadata identifies a logical packet; bytes contains its entire modeled
// message_size-byte payload. The SimpleNetwork request separately specifies
// the number of on-network bits, including Mordred's HEAD/TAIL minimum.
class MordredMeshPayload final : public SST::Event {
public:
    std::uint32_t source = 0, destination = 0, vn = 0;
    std::uint64_t sequence = 0, sendTick = 0;
    std::vector<std::uint8_t> bytes;
    MordredMeshPayload() = default;
    SST::Event* clone() override { return new MordredMeshPayload(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(source); SST_SER(destination); SST_SER(vn);
        SST_SER(sequence); SST_SER(sendTick); SST_SER(bytes);
    }
    ImplementSerializable(TileComponents::MordredMeshPayload);
};

class MordredMeshEndpoint final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(MordredMeshEndpoint, "mordredtests", "meshEndpoint",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Deterministic all-to-all SimpleNetwork payload and credit regression endpoint",
        COMPONENT_CATEGORY_NETWORK)
    SST_ELI_DOCUMENT_PARAMS(
        {"id", "Expected network endpoint ID; row-major mesh ID", "0"},
        {"num_peers", "Total endpoints, including this endpoint", "4"},
        {"num_messages", "Messages to each other endpoint; no self traffic", "16"},
        {"message_size", "Exact modeled message size in bytes; at least two flits", "64"},
        {"flit_size_bits", "Router flit width in bits, used to check packet sizing", "128"},
        {"num_vns", "Virtual networks used by deterministic rotating traffic", "1"},
        {"clock", "Endpoint clock; must match the NIC clock", "1GHz"},
        {"recv_delay_cycles", "Delay between packet arrival notification and recv(); upstream Mordred returns credits on arrival", "0"},
        {"timeout_cycles", "Fatal timeout in endpoint clock cycles", "1000000"},
        {"result_file", "Optional per-endpoint JSON output path", ""})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Finite-buffer network interface", "SST::Interfaces::SimpleNetwork"})

    MordredMeshEndpoint(SST::ComponentId_t id, SST::Params& p) : SST::Component(id),
        id_(p.find<std::uint32_t>("id",0)), peers_(p.find<std::uint32_t>("num_peers",4)),
        messages_(p.find<std::uint64_t>("num_messages",16)),
        messageBytes_(p.find<std::uint64_t>("message_size",64)),
        flitBits_(p.find<std::uint32_t>("flit_size_bits",128)),
        vns_(p.find<std::uint32_t>("num_vns",1)),
        recvDelay_(p.find<std::uint64_t>("recv_delay_cycles",0)),
        timeout_(p.find<std::uint64_t>("timeout_cycles",1000000)),
        clockName_(p.find<std::string>("clock","1GHz")),
        resultFile_(p.find<std::string>("result_file","")) {
        if (peers_ < 2 || id_ >= peers_ || !messages_ || !messageBytes_ ||
            messageBytes_ > std::uint64_t(INT32_MAX)/8 || !flitBits_ ||
            messageBytes_*8 < std::uint64_t(flitBits_)*2 || !vns_ || vns_ > INT32_MAX ||
            !timeout_ || recvDelay_ >= timeout_ ||
            messages_ > std::numeric_limits<std::size_t>::max()/peers_ ||
            messages_ > UINT64_MAX/(peers_-1) ||
            messages_*(peers_-1) > UINT64_MAX/messageBytes_)
            fail("invalid endpoint, packet, virtual-network, delay or count parameter");
        messageBits_ = static_cast<int>(messageBytes_*8);
        flitsPerMessage_ = (messageBytes_*8 + flitBits_-1)/flitBits_;
        expected_ = messages_*(peers_-1);
        if (expected_ > UINT64_MAX/flitsPerMessage_)
            fail("expected network flit count overflows");
        seen_.assign(static_cast<std::size_t>(peers_*messages_),false);
        sentPerPeer_.assign(peers_,0); receivedPerPeer_.assign(peers_,0);
        sentPerVn_.assign(vns_,0); receivedPerVn_.assign(vns_,0);
        arrivals_.resize(vns_);
        network_ = loadUserSubComponent<Network>("networkIF",SST::ComponentInfo::SHARE_NONE,
                                                 static_cast<int>(vns_));
        if (!network_) fail("networkIF SimpleNetwork subcomponent is required");
        network_->setNotifyOnReceive(
            new Network::Handler<MordredMeshEndpoint,&MordredMeshEndpoint::onReceive>(this));
        clock_ = registerClock(clockName_,
            new SST::Clock::Handler<MordredMeshEndpoint,&MordredMeshEndpoint::tick>(this));
        tickNs_ = getCoreTimeBase().getDoubleValue()*1e9;
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void init(unsigned phase) override { network_->init(phase); }
    void setup() override {
        network_->setup();
        if (!network_->isNetworkInitialized() || network_->getEndpointID() != id_)
            fail("network initialization or expected row-major endpoint ID mismatch");
        const auto expectedBandwidth = SST::UnitAlgebra(clockName_) *
            SST::UnitAlgebra(std::to_string(flitBits_)+"b");
        if (network_->getLinkBW() != expectedBandwidth)
            fail("NIC link bandwidth disagrees with endpoint clock and flit_size_bits");
        initialized_ = true;
    }
    void complete(unsigned phase) override { network_->complete(phase); }
    void finish() override {
        if (!done_ || sent_ != expected_ || received_ != expected_ || notifications_ != expected_)
            fail("simulation ended before all expected packets were sent, delivered and checked");
        for (std::uint32_t peer = 0; peer < peers_; ++peer) {
            const auto wanted = peer == id_ ? 0 : messages_;
            if (sentPerPeer_[peer] != wanted || receivedPerPeer_[peer] != wanted)
                fail("incomplete all-to-all peer coverage");
        }
        for (std::uint32_t vn = 0; vn < vns_; ++vn)
            if (!arrivals_[vn].empty() || network_->requestToReceive(vn))
                fail("unconsumed or duplicate receive remains at simulation finish");
        network_->finish();
        writeResult(true,"");
    }

private:
    using Network = SST::Interfaces::SimpleNetwork;
    struct Arrival { std::uint64_t tick, readyTick; };
    Network* network_ = nullptr;
    SST::TimeConverter clock_;
    std::uint32_t id_, peers_;
    std::uint64_t messages_, messageBytes_;
    std::uint32_t flitBits_, vns_;
    std::uint64_t recvDelay_, timeout_;
    std::string clockName_, resultFile_;
    int messageBits_ = 0;
    std::uint64_t expected_ = 0, flitsPerMessage_ = 0;
    std::uint64_t sent_ = 0, received_ = 0, notifications_ = 0, verifiedBytes_ = 0;
    std::uint64_t sendAttempts_ = 0, creditStalls_ = 0, sendRejected_ = 0, delayWaitCycles_ = 0;
    std::uint64_t firstSendTick_ = 0, lastSendTick_ = 0, firstArrivalTick_ = 0, lastArrivalTick_ = 0;
    std::uint64_t lastReceiveTick_ = 0, completionCycle_ = 0;
    std::uint64_t minLatencyTicks_ = UINT64_MAX, maxLatencyTicks_ = 0;
    long double latencyTicksSum_ = 0, receiveTicksSum_ = 0;
    double tickNs_ = 0;
    std::uint64_t payloadChecksum_ = 0;
    bool initialized_ = false, done_ = false;
    std::vector<bool> seen_;
    std::vector<std::uint64_t> sentPerPeer_, receivedPerPeer_, sentPerVn_, receivedPerVn_;
    std::vector<std::deque<Arrival>> arrivals_;

    static std::uint8_t expectedByte(std::uint32_t source, std::uint32_t destination,
                                    std::uint64_t sequence, std::uint32_t vn,
                                    std::uint64_t index) {
        std::uint64_t value = (std::uint64_t(source)<<32) ^ destination ^
            (sequence*UINT64_C(0x9e3779b97f4a7c15)) ^
            (std::uint64_t(vn)*UINT64_C(0xbf58476d1ce4e5b9)) ^
            (index*UINT64_C(0x94d049bb133111eb));
        value ^= value>>30; value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value>>27; value *= UINT64_C(0x94d049bb133111eb);
        value ^= value>>31;
        return static_cast<std::uint8_t>(value);
    }

    bool onReceive(int vn) {
        if (!initialized_ || vn < 0 || static_cast<std::uint32_t>(vn) >= vns_)
            fail("receive notification has invalid initialization state or virtual network");
        const auto tick = getCurrentSimCycle();
        const auto factor = clock_.getFactor();
        if (recvDelay_ > (UINT64_MAX-tick)/factor) fail("receive delay cycle overflow");
        arrivals_[vn].push_back({tick,tick+recvDelay_*factor});
        if (!notifications_) firstArrivalTick_ = tick;
        lastArrivalTick_ = tick;
        ++notifications_;
        if (notifications_ > expected_) fail("extra packet arrival notification");
        // Mordred returns flit credits on arrival, before this endpoint calls
        // recv(). This delay tests endpoint consumption, not NIC credit hold.
        return true;
    }

    bool tick(SST::Cycle_t cycle) {
        if (!initialized_) fail("endpoint clock ran before network initialization");
        if (cycle > timeout_ && !done_) fail("timed out waiting for exact all-to-all completion");
        for (std::uint32_t vn = 0; vn < vns_; ++vn) {
            auto& arrivals = arrivals_[vn];
            while (!arrivals.empty() && arrivals.front().readyTick <= getCurrentSimCycle()) {
                const auto arrival = arrivals.front();
                receive(vn,arrival);
                arrivals.pop_front();
            }
            if (!arrivals.empty()) ++delayWaitCycles_;
        }
        if (sent_ < expected_) send();
        if (!done_ && sent_ == expected_ && received_ == expected_) {
            done_ = true;
            completionCycle_ = cycle;
            primaryComponentOKToEndSim();
        }
        // Remain able to reject extra arrivals while other endpoints finish.
        return false;
    }

    void send() {
        const auto sequence = sent_/(peers_-1);
        const auto destination = static_cast<std::uint32_t>(
            (std::uint64_t(id_)+1+sent_%(peers_-1))%peers_);
        const auto vn = static_cast<std::uint32_t>((sequence+id_+destination)%vns_);
        ++sendAttempts_;
        if (!network_->spaceToSend(vn,messageBits_)) { ++creditStalls_; return; }
        auto packet = std::make_unique<MordredMeshPayload>();
        packet->source = id_; packet->destination = destination; packet->sequence = sequence;
        packet->vn = vn; packet->sendTick = getCurrentSimCycle();
        packet->bytes.resize(messageBytes_);
        for (std::uint64_t i = 0; i < messageBytes_; ++i)
            packet->bytes[i] = expectedByte(id_,destination,sequence,vn,i);
        auto request = std::make_unique<Network::Request>(destination,id_,messageBits_,
                                                          true,true,packet.release());
        request->vn = vn;
        request->allow_adaptive = false;
        if (!network_->send(request.get(),vn)) { ++sendRejected_; return; }
        request.release(); // NIC owns the request and its payload after acceptance.
        if (!sent_) firstSendTick_ = getCurrentSimCycle();
        lastSendTick_ = getCurrentSimCycle();
        ++sent_; ++sentPerPeer_[destination]; ++sentPerVn_[vn];
    }

    void receive(std::uint32_t vn, Arrival arrival) {
        std::unique_ptr<Network::Request> request(network_->recv(vn));
        if (!request) fail("receive notification did not correspond to a queued packet");
        auto* packet = dynamic_cast<MordredMeshPayload*>(request->inspectPayload());
        if (!packet || request->src < 0 || request->src >= peers_ || request->src == id_ ||
            request->dest != id_ || request->vn != static_cast<int>(vn) ||
            request->size_in_bits != static_cast<std::size_t>(messageBits_) ||
            !request->head || !request->tail || request->allow_adaptive ||
            packet->source != request->src || packet->destination != id_ || packet->vn != vn ||
            packet->sequence >= messages_ || packet->bytes.size() != messageBytes_ ||
            vn != (packet->sequence+packet->source+id_)%vns_ ||
            packet->sendTick > arrival.tick || arrival.tick > getCurrentSimCycle())
            fail("packet source, destination, sequence, VN, size, timing or payload type mismatch");
        const auto index = static_cast<std::size_t>(packet->source*messages_+packet->sequence);
        if (seen_[index]) fail("duplicate source/sequence packet");
        std::uint64_t digest = UINT64_C(14695981039346656037);
        for (std::uint64_t i = 0; i < messageBytes_; ++i) {
            if (packet->bytes[i] != expectedByte(packet->source,id_,packet->sequence,vn,i))
                fail("payload byte corruption");
            digest = (digest ^ packet->bytes[i])*UINT64_C(1099511628211);
        }
        payloadChecksum_ ^= digest; // Independent of inter-VN arrival ordering.
        seen_[index] = true;
        ++received_; ++receivedPerPeer_[packet->source]; ++receivedPerVn_[vn];
        verifiedBytes_ += messageBytes_;
        const auto latency = arrival.tick-packet->sendTick;
        minLatencyTicks_ = std::min(minLatencyTicks_,latency);
        maxLatencyTicks_ = std::max(maxLatencyTicks_,latency);
        latencyTicksSum_ += latency;
        lastReceiveTick_ = getCurrentSimCycle();
        receiveTicksSum_ += lastReceiveTick_-packet->sendTick;
    }

    static void array(std::ostream& out,const std::vector<std::uint64_t>& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) out << (i ? "," : "") << values[i];
        out << ']';
    }
    void writeResult(bool passed,const std::string& error) {
        std::ostringstream out;
        out << std::setprecision(17) << "{\"passed\":" << (passed ? "true" : "false")
            << ",\"id\":" << id_ << ",\"num_peers\":" << peers_
            << ",\"num_messages_per_peer\":" << messages_ << ",\"message_bytes\":" << messageBytes_
            << ",\"flit_size_bits\":" << flitBits_ << ",\"flits_per_message\":" << flitsPerMessage_
            << ",\"num_vns\":" << vns_ << ",\"expected_messages\":" << expected_
            << ",\"sent\":" << sent_ << ",\"received\":" << received_
            << ",\"arrival_notifications\":" << notifications_ << ",\"verified_bytes\":" << verifiedBytes_
            << ",\"expected_flits\":" << expected_*flitsPerMessage_
            << ",\"send_attempts\":" << sendAttempts_ << ",\"credit_stall_cycles\":" << creditStalls_
            << ",\"send_blocked_cycles\":" << creditStalls_+sendRejected_
            << ",\"send_rejections\":" << sendRejected_ << ",\"recv_delay_cycles\":" << recvDelay_
            << ",\"recv_delay_wait_vn_cycles\":" << delayWaitCycles_
            << ",\"first_send_tick\":" << firstSendTick_ << ",\"last_send_tick\":" << lastSendTick_
            << ",\"first_arrival_tick\":" << firstArrivalTick_ << ",\"last_arrival_tick\":" << lastArrivalTick_
            << ",\"last_receive_tick\":" << lastReceiveTick_ << ",\"completion_cycle\":" << completionCycle_
            << ",\"core_tick_ns\":" << tickNs_
            << ",\"delivery_latency_ns_min\":" << (received_ ? minLatencyTicks_*tickNs_ : 0)
            << ",\"delivery_latency_ns_max\":" << maxLatencyTicks_*tickNs_
            << ",\"delivery_latency_ns_mean\":" << (received_ ? latencyTicksSum_*tickNs_/received_ : 0)
            << ",\"receive_latency_ns_mean\":" << (received_ ? receiveTicksSum_*tickNs_/received_ : 0)
            << ",\"payload_checksum_xor\":" << payloadChecksum_ << ",\"sent_per_peer\":";
        array(out,sentPerPeer_); out << ",\"received_per_peer\":"; array(out,receivedPerPeer_);
        out << ",\"sent_per_vn\":"; array(out,sentPerVn_);
        out << ",\"received_per_vn\":"; array(out,receivedPerVn_);
        out << ",\"error\":" << std::quoted(error) << '}';
        if (!resultFile_.empty()) {
            std::ofstream file(resultFile_);
            if (!file) fatal(CALL_INFO,-1,"cannot open Mordred endpoint result file %s\n",resultFile_.c_str());
            file << out.str() << '\n';
            if (!file) fatal(CALL_INFO,-1,"cannot write Mordred endpoint result file %s\n",resultFile_.c_str());
        }
        std::cout << "MORDRED_ENDPOINT_RESULT " << out.str() << '\n';
    }
    [[noreturn]] void fail(const std::string& message) {
        writeResult(false,message);
        fatal(CALL_INFO,-1,"Mordred endpoint %u: %s\n",id_,message.c_str());
        std::abort();
    }
};

} // namespace TileComponents
