#pragma once
#include "guestNetwork.h"
#include "networkCommand.h"
#include "messagePacket.h"
#include <sst/core/params.h>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

namespace TileComponents {
class MordredSpmEndpoint;
// The guest control portion of the NIU. All payload and descriptor accesses use
// the owning endpoint's timed, bank-permission-checked StandardMem service.
class NetworkEngine {
public:
    NetworkEngine(MordredSpmEndpoint&, SST::Params&);
    void init();
    void setup();
    void configuration(const NetworkSlotControl&);
    void command(std::unique_ptr<NetworkCommand>);
    void response(std::unique_ptr<MordredMessagePacket>);
    void control(const NetworkSlotControl&);
    void admit(const MordredMessagePacket&);
    void committed(const MordredMessagePacket&);
    void tick();
    bool sendControl();
    bool idle() const;
    void finish();
    std::uint64_t controlSent=0, controlReceived=0;
private:
    using Packet=MordredMessagePacket;
    enum SlotState { Free, Filling, Ready, Held };
    struct Slot {
        SlotState state=Free;
        std::uint64_t generation=1, sequence=0, bytes=0, invocation=0, admitted=0, committed=0;
    };
    struct Transfer {
        bool advertised=false, configurationSent=false, eligible=false;
        std::uint32_t source=0, destination=0, count=0, slotBase=0;
        std::uint64_t id=0, address=0, capacity=0;
        std::vector<Slot> slots;
        std::vector<std::uint64_t> remoteGeneration;
        std::vector<bool> remoteAvailable;
        std::deque<std::uint32_t> available;
        std::map<std::uint64_t,std::uint32_t> ready;
        std::uint64_t nextSend=1, nextReserve=1, nextReceive=1;
    };
    struct Ticket {
        bool live=false, complete=false;
        std::uint64_t generation=1;
        std::int64_t status=0;
    };
    struct Send {
        std::uint32_t ticket=0, transfer=0, slot=UINT32_MAX;
        std::uint64_t generation=0, sequence=0, issued=0, posted=0;
        GolemNetDescriptor descriptor{};
        std::map<std::uint64_t,std::vector<std::uint8_t>> captured;
    };
    struct Read { bool metadata=false; std::uint32_t ticket=0; std::uint64_t offset=0; };
    struct Metadata {
        std::uint64_t address=0, issued=0, completed=0;
        std::uint32_t ticket=UINT32_MAX;
        std::vector<std::uint8_t> data;
    };
    MordredSpmEndpoint& endpoint_;
    std::uint32_t commandLimit_, commandCount_=0, buffered_=0, cursor_=0;
    std::vector<Transfer> transfers_;
    std::map<std::uint64_t,std::uint32_t> byTransfer_;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> receiveSlots_;
    std::deque<std::uint32_t> eligible_;
    std::vector<Ticket> tickets_;
    std::deque<std::uint32_t> freeTickets_;
    std::map<std::uint32_t,Send> sends_;
    std::map<std::uint64_t,Read> reads_;
    std::deque<std::unique_ptr<NetworkSlotControl>> controls_;
    std::unique_ptr<NetworkCommand> cpu_;
    std::unique_ptr<Metadata> metadata_;
    std::uint64_t requestId_=0;
    std::uint64_t submitted_=0, sourceComplete_=0, received_=0, released_=0, wouldBlock_=0;
    std::uint64_t maxCommands_=0, maxTickets_=0, maxBuffered_=0, maxSlots_=0;
    std::uint64_t descriptorBytes_=0, payloadBytes_=0, receiveWaitCycles_=0, sendWaitCycles_=0;
    std::ofstream trace_;
    static constexpr std::uint64_t TxKind=UINT64_C(1)<<61, RxKind=UINT64_C(1)<<62;
    static constexpr std::uint64_t GenerationLimit=(UINT64_C(1)<<45)-1;
    std::uint64_t identity(std::uint64_t kind, std::uint64_t generation, std::uint32_t slot) const;
    bool ticketIndex(std::uint64_t, std::uint32_t&) const;
    bool slotIndex(std::uint64_t, std::uint32_t&, std::uint32_t&) const;
    int range(std::uint64_t address, std::uint64_t bytes) const;
    std::uint64_t nextRequest();
    void reply(std::int64_t);
    void checkWait();
    void makeEligible(std::uint32_t);
    void metadataComplete();
    void retire(std::uint32_t);
    void queueControl(std::unique_ptr<NetworkSlotControl>);
    void log(const char*, std::uint64_t=0, std::uint64_t=0, std::uint64_t=0, std::uint64_t=0, std::uint64_t=0);
};
}
