#pragma once
#include "../cycleProfile.h"
#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/link.h>
#include "messagePacket.h"
#include "networkEngine.h"
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <vector>

namespace TileComponents {
class MordredSpmEndpoint : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(MordredSpmEndpoint, "tilecomponents", "MordredSpmEndpoint",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Bounded router-facing interface to timed banked SPM", COMPONENT_CATEGORY_NETWORK)
    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Row-major endpoint ID", "0"},
        {"tile_count", "Number of endpoints sharing the protocol", "4"},
        {"spm_capacity_bytes", "Local tile SPM capacity", "2097152"},
        {"spm_request_bytes", "SPM request boundary and maximum fragment size", "32"},
        {"spm_banks", "Total physical SPM banks", "4"},
        {"spm_bank_width", "Address stripe and bank-port bytes", "4"},
        {"router_spm_banks", "Physical bank IDs accessible to router; default highest two", ""},
        {"request_window", "Maximum in-flight payload chunks across local reads and NIC admission", "4"},
        {"max_request_bytes", "Maximum payload bytes per message packet", "256"},
        {"memory_queue_depth", "Maximum outstanding StandardMem fragments", "8"},
        {"posted_receive_slots_per_source", "Reserved posted-write payload slots per remote source; zero disables", "16"},
        {"posted_credit_batch", "Maximum committed slots per credit packet; capped to receive slots", "4"},
        {"posted_credit_delay_cycles", "Flush partial credit batches after this many cycles", "4"},
        {"net_command_queue_depth", "Active whole-message send descriptors", "4"},
        {"net_ticket_capacity", "Live send tickets including unretired completions", "16"},
        {"net_transfers", "Deployment records: transfer ID, source tile, destination tile, receive base, slot capacity, slot count", "[]"},
        {"flit_size_bits", "Mordred flit width", "128"},
        {"clock", "Endpoint and NIC clock", "1GHz"})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Timed requests to the tile's shared SPM", "SST::Interfaces::StandardMem"},
        {"networkIF", "Mordred SimpleNetwork endpoint", "SST::Interfaces::SimpleNetwork"})
    SST_ELI_DOCUMENT_PORTS(
        {"network_commands", "Guest NIU command and completion control path", {"TileComponents.NetworkCommand"}})
    MordredSpmEndpoint(SST::ComponentId_t, SST::Params&);
    void init(unsigned) override;
    void setup() override;
    void complete(unsigned) override;
    void finish() override;
protected:
    using Memory = SST::Interfaces::StandardMem;
    using Packet = MordredMessagePacket;
    // Specialize only the local payload source. Descriptors, incoming payloads,
    // routing, ordering and credits continue to use the common NIU machinery.
    virtual std::uint64_t sourceBase() const { return UINT64_C(0x90000000); }
    virtual bool payloadUsesSpm() const { return true; }
    virtual int sourceRange(std::uint64_t, std::uint64_t) const;
    virtual std::uint32_t memoryBoundary(const Packet&) const { return requestBytes_; }
    virtual void sendMemory(const Packet&, Memory::Request* request) { memory_->send(request); }
    int spmRange(std::uint64_t, std::uint64_t) const;
    void memoryResponse(Memory::Request*);
    SST::TimeConverter endpointClock() const { return clock_; }
    [[noreturn]] void fail(const char*);
private:
    friend class NetworkEngine;
    using Network = SST::Interfaces::SimpleNetwork;
    using Key = std::pair<std::uint32_t, std::uint64_t>;
    struct Job { std::unique_ptr<Packet> packet; std::size_t issued=0, completed=0; bool localBank=false; };
    struct Pending { std::shared_ptr<Job> job; std::size_t offset, bytes; };
    struct Peer {
        bool seen=false;
        std::uint64_t capacity=0;
        std::uint32_t banks=0, bankWidth=0, maxBytes=0, slots=0, credits=0;
        std::vector<bool> allowed;
    };
    Memory* memory_ = nullptr;
    Network* network_ = nullptr;
    SST::Link* commands_ = nullptr;
    std::unique_ptr<NetworkEngine> net_;
    SST::TimeConverter clock_;
    std::string clockName_;
    std::uint32_t tile_, tiles_, requestBytes_, banks_, bankWidth_, window_, maxBytes_, memoryDepth_, flitBits_;
    std::uint32_t postedSlots_, creditBatch_, creditDelay_;
    std::uint64_t capacity_;
    bool initialized_=false, holding_=false, advertised_=false;
    std::vector<bool> allowedBanks_;
    std::vector<Peer> peers_;
    std::vector<std::uint32_t> postedReservations_, pendingCredits_;
    std::vector<std::uint64_t> creditSince_, receivedPostedId_;
    std::vector<std::uint32_t> postedSendSequence_, postedReceiveSequence_;
    std::vector<std::map<std::uint32_t, std::shared_ptr<Job>>> postedReorder_;
    std::uint64_t lastPostedId_=0;
    std::uint32_t nextCreditPeer_=0, metadataRequests_=0;
    std::set<std::uint64_t> local_;
    std::map<Key, std::shared_ptr<Job>> incoming_;
    std::deque<std::shared_ptr<Job>> ready_;
    std::deque<std::unique_ptr<Packet>> outgoing_;
    std::map<Memory::Request::id_t, Pending> pending_;
    std::ofstream trace_;
    CycleProfile cycleProfile_;
    std::uint64_t reads_=0, writes_=0;
    std::uint64_t localBankCompleted_=0, localBankRejected_=0;
    std::uint64_t localRejected_=0, requestsSent_=0;
    std::uint64_t requestsReceived_=0, wireSent_=0, wireReceived_=0;
    std::uint64_t memoryStalls_=0, networkStalls_=0, maxPending_=0, maxIncoming_=0, maxLocal_=0;
    std::uint64_t packetsInNic_=0, packetsInjected_=0, maxPendingNic_=0;
    std::uint64_t postedAccepted_=0, postedCommitted_=0;
    std::uint64_t creditPacketsSent_=0, creditPacketsReceived_=0, creditsSent_=0, creditsReceived_=0;
    std::uint64_t creditWireSent_=0, creditWireReceived_=0, postedCreditStalls_=0;
    std::uint64_t maxPostedReservations_=0, maxPendingCredits_=0, maxReservedCredits_=0;
    std::uint64_t reorderedPackets_=0, maxReorderPending_=0, reorderWaitCycles_=0;
    bool tick(SST::Cycle_t);
    bool receiveNotification(int);
    bool sendNotification(int);
    void localRequest(std::unique_ptr<Packet>);
    void guestCommand(SST::Event*);
    void receivePackets();
    void receiveRequest(std::unique_ptr<Packet>);
    void receiveCredit(const MordredSpmCredit&);
    bool sendCredit();
    void sendPacket();
    void pumpMemory();
    void returnLocal(std::unique_ptr<Packet>, std::uint32_t);
    std::uint32_t validate(const Packet&, bool targetAccess=true) const;
    std::uint32_t validatePosted(const Packet&) const;
    std::uint64_t wireBytes(const Packet&) const;
    std::uint64_t creditWireBytes() const;
    void updateHold();
    bool idle() const;
    void trace(const char*, const Packet&, std::uint64_t=0, std::uint64_t=0, std::uint64_t=0);
};
}
