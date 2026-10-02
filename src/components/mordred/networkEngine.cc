#include <sst/core/sst_config.h>
#include "networkEngine.h"
#include "spmEndpoint.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>

namespace TileComponents {
namespace {
constexpr std::uint64_t SpmBase=UINT64_C(0x90000000);
std::uint64_t word(const std::vector<std::uint8_t>& data, std::size_t index) {
    std::uint64_t value=0;
    for (unsigned byte=0; byte<8; ++byte) value|=std::uint64_t(data.at(index*8+byte))<<(byte*8);
    return value;
}
}

NetworkEngine::NetworkEngine(MordredSpmEndpoint& endpoint, SST::Params& params)
    : endpoint_(endpoint), commandLimit_(params.find<std::uint32_t>("net_command_queue_depth",4)) {
    const auto tickets=params.find<std::uint32_t>("net_ticket_capacity",16);
    if (!commandLimit_ || commandLimit_>1024 || tickets<commandLimit_ || tickets>1024)
        endpoint_.fail("invalid guest network command/ticket capacity");
    tickets_.resize(tickets);
    for (std::uint32_t i=0; i<tickets; ++i) freeTickets_.push_back(i);
    // Deployment records: ID, producer, consumer, receive base, capacity, count.
    // Only locally active transfers are installed. IDs are opaque, never indices.
    std::vector<std::uint64_t> records;
    params.find_array<std::uint64_t>("net_transfers",records);
    if (records.size()%6 || records.size()/6>1024)
        endpoint_.fail("net_transfers requires at most 1024 six-word deployment records");
    for (std::size_t n=0; n<records.size(); n+=6) {
        const auto id=records[n], source=records[n+1], destination=records[n+2];
        const auto address=records[n+3], capacity=records[n+4], count=records[n+5];
        if (id>INT64_MAX || source>=endpoint_.tiles_ || destination>=endpoint_.tiles_ ||
            source==destination || (source!=endpoint_.tile_ && destination!=endpoint_.tile_) ||
            !count || count>256 || !capacity || capacity>INT64_MAX || address<SpmBase ||
            address>INT64_MAX || address%8 || capacity> (INT64_MAX-address)/count ||
            !endpoint_.postedSlots_ || byTransfer_.count(id))
            endpoint_.fail("invalid or duplicate guest transfer deployment");
        Transfer c;
        c.id=id; c.source=source; c.destination=destination;
        c.address=address; c.capacity=capacity; c.count=count;
        const auto index=transfers_.size();
        if (destination==endpoint_.tile_) {
            if (range(address,capacity*count) || receiveSlots_.size()+count>65536)
                endpoint_.fail("receive reservation exceeds accessible SPM or token capacity");
            for (const auto& other:transfers_)
                if (other.destination==destination && address<other.address+other.capacity*other.count &&
                    other.address<address+capacity*count)
                    endpoint_.fail("transfer receive reservations overlap");
            c.slotBase=receiveSlots_.size(); c.slots.resize(count);
            for (std::uint32_t slot=0; slot<count; ++slot) receiveSlots_.emplace_back(index,slot);
        }
        byTransfer_.emplace(id,index); transfers_.push_back(std::move(c));
    }
    if (const auto dir=CycleProfile::traceDirectory(); !dir.empty()) {
        trace_.open(dir+"/"+endpoint_.getName()+"-messages.csv");
        if (!trace_) endpoint_.fail("cannot open guest network trace");
        trace_ << "event,cycle,transfer_id,identity,offset,bytes,invocation_id\n";
    }
}

void NetworkEngine::init() {
    for (auto& c:transfers_) {
        if (c.destination!=endpoint_.tile_ || c.configurationSent || !endpoint_.peers_[c.source].seen) continue;
        c.configurationSent=true;
        if (!endpoint_.peers_[c.source].slots) endpoint_.fail("transfer producer lacks guest messaging");
        auto* config=new NetworkSlotControl;
        config->source=c.destination; config->destination=c.source; config->transferId=c.id;
        config->address=c.address; config->capacity=config->stride=c.capacity; config->slots=c.count;
        auto* request=new MordredSpmEndpoint::Network::Request(c.source,endpoint_.tile_,0,true,true,config);
        request->vn=0; request->allow_adaptive=false;
        endpoint_.network_->sendUntimedData(request);
    }
}
void NetworkEngine::configuration(const NetworkSlotControl& config) {
    const auto found=byTransfer_.find(config.transferId);
    if (config.kind!=NetworkSlotControl::Configuration || config.protocol!=2 || config.reserved ||
        config.destination!=endpoint_.tile_ || config.generation!=1 || config.slot || found==byTransfer_.end())
        endpoint_.fail("invalid transfer receive configuration");
    auto& c=transfers_[found->second];
    if (c.source!=endpoint_.tile_ || config.source!=c.destination || c.advertised ||
        config.slots!=c.count || config.capacity!=c.capacity || config.stride!=c.capacity || config.address!=c.address)
        endpoint_.fail("transfer producer/consumer deployment mismatch");
    c.advertised=true;
    c.remoteGeneration.assign(c.count,1); c.remoteAvailable.assign(c.count,true);
    for (std::uint32_t slot=0; slot<c.count; ++slot) c.available.push_back(slot);
}
void NetworkEngine::setup() {
    for (const auto& c:transfers_) {
        if (c.source!=endpoint_.tile_) continue;
        const auto& peer=endpoint_.peers_[c.destination];
        if (!c.advertised || !peer.slots) endpoint_.fail("missing transfer receive configuration");
        if (c.address-SpmBase>=peer.capacity || c.capacity>peer.capacity/c.count ||
            c.capacity*c.count>peer.capacity-(c.address-SpmBase))
            endpoint_.fail("transfer receive slots exceed destination SPM");
        auto begin=c.address-SpmBase;
        auto remaining=std::min<std::uint64_t>(c.capacity*c.count,std::uint64_t(peer.banks)*peer.bankWidth);
        while (remaining) {
            if (!peer.allowed[(begin/peer.bankWidth)%peer.banks]) endpoint_.fail("transfer crosses a forbidden destination bank");
            const auto bytes=std::min<std::uint64_t>(remaining,peer.bankWidth-begin%peer.bankWidth);
            begin+=bytes; remaining-=bytes;
        }
    }
}

std::uint64_t NetworkEngine::identity(std::uint64_t kind, std::uint64_t generation,
                                     std::uint32_t index) const {
    if (!generation || generation>GenerationLimit || index>65535)
        endpoint_.fail("network identity generation exhausted");
    return kind|(generation<<16)|index;
}
bool NetworkEngine::ticketIndex(std::uint64_t token, std::uint32_t& index) const {
    index=token&65535;
    return index<tickets_.size() && tickets_[index].live &&
        token==identity(TxKind,tickets_[index].generation,index);
}
bool NetworkEngine::slotIndex(std::uint64_t token, std::uint32_t& transfer,
                              std::uint32_t& index) const {
    const auto flat=token&65535;
    if (flat>=receiveSlots_.size()) return false;
    transfer=receiveSlots_[flat].first; index=receiveSlots_[flat].second;
    const auto& slot=transfers_[transfer].slots[index];
    return slot.state==Held && token==identity(RxKind,slot.generation,flat);
}
int NetworkEngine::range(std::uint64_t address, std::uint64_t bytes) const {
    return endpoint_.spmRange(address,bytes);
}
std::uint64_t NetworkEngine::nextRequest() {
    if (requestId_==UINT64_MAX) endpoint_.fail("network request IDs exhausted");
    return ++requestId_;
}
void NetworkEngine::log(const char* event, std::uint64_t transfer, std::uint64_t id,
                        std::uint64_t offset, std::uint64_t bytes, std::uint64_t invocation) {
    if (trace_) trace_ << event << ',' << endpoint_.getCurrentSimTime(endpoint_.clock_) << ','
        << transfer << ',' << id << ',' << offset << ',' << bytes << ',' << invocation << '\n';
}
void NetworkEngine::reply(std::int64_t result) {
    if (!cpu_) endpoint_.fail("guest network completion has no command");
    log("reply",0,cpu_->token,std::uint64_t(result));
    cpu_->result=result; cpu_->response=true;
    endpoint_.commands_->send(cpu_.release());
}
void NetworkEngine::retire(std::uint32_t index) {
    auto& ticket=tickets_.at(index);
    if (!ticket.live || ticket.generation==GenerationLimit) endpoint_.fail("invalid network ticket retirement");
    ticket.live=ticket.complete=false; ticket.status=0; ++ticket.generation;
    freeTickets_.push_back(index);
}

void NetworkEngine::command(std::unique_ptr<NetworkCommand> command) {
    if (cpu_ || command->response) endpoint_.fail("overlapping guest network control commands");
    cpu_=std::move(command);
    const auto op=cpu_->operation;
    log("command",0,cpu_->token,op);
    if (op==GOLEM_NET_SEND) {
        if (cpu_->first>=endpoint_.tiles_ || cpu_->first==endpoint_.tile_ || cpu_->second%8) {
            reply(GOLEM_NET_INVALID); return;
        }
        if (!endpoint_.postedSlots_ || !endpoint_.peers_[cpu_->first].slots) {
            reply(GOLEM_NET_UNAVAILABLE); return;
        }
        if (const auto error=range(cpu_->second,sizeof(GolemNetDescriptor))) { reply(error); return; }
        // Admission never waits behind retained completions. One descriptor
        // read has a reserved request slot so payload backpressure cannot trap
        // the CPU inside an admitted descriptor capture.
        if (commandCount_==commandLimit_ || freeTickets_.empty()) {
            ++wouldBlock_; reply(GOLEM_NET_WOULD_BLOCK); return;
        }
        metadata_=std::make_unique<Metadata>();
        metadata_->address=cpu_->second; metadata_->data.resize(sizeof(GolemNetDescriptor));
        const auto index=freeTickets_.front(); freeTickets_.pop_front();
        metadata_->ticket=index; tickets_[index].live=true;
        ++commandCount_;
        maxCommands_=std::max<std::uint64_t>(maxCommands_,commandCount_);
        maxTickets_=std::max<std::uint64_t>(maxTickets_,tickets_.size()-freeTickets_.size());
        tick();
        return;
    }
    if (op==GOLEM_NET_RECV || op==GOLEM_NET_TRY_RECV || op==GOLEM_NET_WAIT || op==GOLEM_NET_TRY_WAIT) {
        if (cpu_->second || ((op==GOLEM_NET_RECV || op==GOLEM_NET_TRY_RECV) && cpu_->first)) {
            reply(GOLEM_NET_INVALID); return;
        }
        checkWait(); return;
    }
    if (op==GOLEM_NET_INFO || op==GOLEM_NET_RELEASE) {
        std::uint32_t transfer,index;
        if (!slotIndex(cpu_->first,transfer,index)) { reply(GOLEM_NET_STALE); return; }
        auto& c=transfers_[transfer]; auto& slot=c.slots[index];
        if (op==GOLEM_NET_INFO) {
            switch (cpu_->second) {
            case GOLEM_NET_POINTER: reply(c.address+index*c.capacity); break;
            case GOLEM_NET_LENGTH: reply(slot.bytes); break;
            case GOLEM_NET_SEQUENCE: reply(slot.sequence); break;
            case GOLEM_NET_TRANSFER_ID: reply(c.id); break;
            case GOLEM_NET_INVOCATION_ID: reply(slot.invocation); break;
            case GOLEM_NET_SOURCE: reply(c.source); break;
            default: reply(GOLEM_NET_INVALID); break;
            }
            return;
        }
        if (cpu_->second) { reply(GOLEM_NET_INVALID); return; }
        log("release",c.id,cpu_->first,index,slot.bytes,slot.invocation);
        if (slot.generation==GenerationLimit) endpoint_.fail("receive slot generation exhausted");
        slot.state=Free; ++slot.generation; ++released_;
        auto credit=std::make_unique<NetworkSlotControl>();
        credit->kind=NetworkSlotControl::Credit; credit->source=endpoint_.tile_;
        credit->destination=c.source; credit->transferId=c.id; credit->slot=index;
        credit->generation=slot.generation;
        queueControl(std::move(credit));
        reply(0); return;
    }
    reply(GOLEM_NET_INVALID);
}

void NetworkEngine::checkWait() {
    if (!cpu_ || metadata_) return;
    const auto op=cpu_->operation;
    if (op==GOLEM_NET_WAIT || op==GOLEM_NET_TRY_WAIT) {
        std::uint32_t index;
        if (!ticketIndex(cpu_->first,index)) { reply(GOLEM_NET_STALE); return; }
        const auto& ticket=tickets_[index];
        if (!ticket.complete) {
            if (op==GOLEM_NET_TRY_WAIT) reply(GOLEM_NET_PENDING);
            return;
        }
        const auto result=ticket.status;
        log("retire",0,cpu_->first); retire(index); reply(result); return;
    }
    if (op!=GOLEM_NET_RECV && op!=GOLEM_NET_TRY_RECV) return;
    if (eligible_.empty()) {
        if (op==GOLEM_NET_TRY_RECV) reply(GOLEM_NET_EMPTY);
        return;
    }
    const auto transfer=eligible_.front(); eligible_.pop_front();
    auto& c=transfers_[transfer]; c.eligible=false;
    const auto found=c.ready.find(c.nextReceive);
    if (found==c.ready.end()) endpoint_.fail("ineligible receive queued");
    const auto index=found->second;
    auto& slot=c.slots[index];
    if (slot.state!=Ready || slot.committed!=slot.bytes) endpoint_.fail("incomplete receive acquired");
    slot.state=Held; c.ready.erase(found); ++c.nextReceive; ++received_;
    makeEligible(transfer);
    const auto token=identity(RxKind,slot.generation,c.slotBase+index);
    log("acquire",c.id,token,index,slot.bytes,slot.invocation);
    reply(token);
}
void NetworkEngine::makeEligible(std::uint32_t transfer) {
    auto& c=transfers_[transfer];
    if (!c.eligible && c.ready.count(c.nextReceive)) {
        c.eligible=true; eligible_.push_back(transfer);
    }
}

void NetworkEngine::metadataComplete() {
    const auto& data=metadata_->data;
    GolemNetDescriptor descriptor{word(data,0),word(data,1),word(data,2),word(data,3)};
    auto found=byTransfer_.find(descriptor.transfer_id);
    std::int64_t result=0;
    if (descriptor.transfer_id>INT64_MAX || descriptor.invocation_id>INT64_MAX || found==byTransfer_.end())
        result=GOLEM_NET_INVALID;
    else {
        const auto& c=transfers_[found->second];
        if (c.source!=endpoint_.tile_ || c.destination!=cpu_->first || !c.advertised ||
            descriptor.bytes>c.capacity || c.nextSend>INT64_MAX)
            result=GOLEM_NET_INVALID;
        else result=endpoint_.sourceRange(descriptor.source_address,descriptor.bytes);
    }
    if (result) { --commandCount_; retire(metadata_->ticket); }
    else {
        auto& c=transfers_[found->second];
        Send send;
        send.ticket=metadata_->ticket; send.transfer=found->second; send.descriptor=descriptor;
        send.sequence=c.nextSend++;
        sends_.emplace(send.ticket,std::move(send)); ++submitted_;
        result=identity(TxKind,tickets_[metadata_->ticket].generation,metadata_->ticket);
        log("submit",c.id,result,descriptor.source_address,descriptor.bytes,descriptor.invocation_id);
    }
    metadata_.reset(); reply(result);
}

void NetworkEngine::response(std::unique_ptr<Packet> packet) {
    if (packet->write) {
        if (packet->status!=Packet::Accepted || !buffered_) endpoint_.fail("guest payload was not admitted");
        --buffered_; return;
    }
    auto found=reads_.find(packet->requestId);
    if (found==reads_.end() || packet->status!=Packet::Success || packet->data.size()!=packet->bytes)
        endpoint_.fail("invalid guest NIU SPM read response");
    const auto read=found->second; reads_.erase(found);
    if (read.metadata) {
        if (!metadata_ || read.offset+packet->bytes>metadata_->data.size()) endpoint_.fail("descriptor read out of bounds");
        std::copy(packet->data.begin(),packet->data.end(),metadata_->data.begin()+read.offset);
        metadata_->completed+=packet->bytes; descriptorBytes_+=packet->bytes;
        if (metadata_->completed==metadata_->data.size()) metadataComplete();
    } else {
        auto& send=sends_.at(read.ticket);
        payloadBytes_+=packet->bytes;
        if (!send.captured.emplace(read.offset,std::move(packet->data)).second)
            endpoint_.fail("duplicate source payload capture");
        log("source_read",send.descriptor.transfer_id,identity(TxKind,tickets_[read.ticket].generation,read.ticket),
            read.offset,packet->bytes,send.descriptor.invocation_id);
    }
}

void NetworkEngine::tick() {
    auto& ep=endpoint_;
    if (cpu_ && !metadata_) {
        if (cpu_->operation==GOLEM_NET_RECV) ++receiveWaitCycles_;
        if (cpu_->operation==GOLEM_NET_WAIT) ++sendWaitCycles_;
    }
    // Descriptor reads have priority so payload backpressure cannot trap the
    // CPU inside an admitted command's descriptor fetch.
    if (metadata_ && metadata_->issued<metadata_->data.size() && metadata_->issued==metadata_->completed) {
        auto p=std::make_unique<Packet>();
        p->metadata=true; p->requestId=nextRequest(); p->destinationTile=ep.tile_;
        p->address=metadata_->address-SpmBase+metadata_->issued;
        p->bytes=std::min<std::uint64_t>(ep.maxBytes_,metadata_->data.size()-metadata_->issued);
        reads_.emplace(p->requestId,Read{true,0,metadata_->issued}); metadata_->issued+=p->bytes;
        ep.localRequest(std::move(p));
    }
    // Packet buffers remain bounded across local reads, captured responses,
    // and packets waiting at NIC admission. Each active message can use several.
    for (std::uint32_t step=0; !sends_.empty() && step<tickets_.size(); ++step) {
        const auto index=std::uint32_t((cursor_+step)%tickets_.size());
        auto found=sends_.find(index);
        if (found==sends_.end()) continue;
        auto& send=found->second; auto& c=transfers_[send.transfer];
        if (send.slot==UINT32_MAX) {
            // Reserve slots in transfer submission order. Otherwise a later message
            // could own the last slot while recv waits for its predecessor.
            if (send.sequence!=c.nextReserve || c.available.empty()) continue;
            send.slot=c.available.front(); c.available.pop_front();
            ++c.nextReserve;
            if (!c.remoteAvailable[send.slot]) ep.fail("application slot credit reused");
            c.remoteAvailable[send.slot]=false; send.generation=c.remoteGeneration[send.slot];
        }
        auto captured=send.captured.find(send.posted);
        if (captured!=send.captured.end() && ep.local_.size()<ep.window_) {
            auto p=std::make_unique<Packet>();
            p->write=true; p->requestId=nextRequest();
            p->destinationTile=c.destination; p->transferId=c.id; p->invocationId=send.descriptor.invocation_id;
            p->messageSlot=send.slot;
            p->messageGeneration=send.generation; p->messageSequence=send.sequence;
            p->messageOffset=send.posted; p->messageBytes=send.descriptor.bytes;
            p->address=c.address-SpmBase+send.slot*c.capacity+send.posted;
            p->data=std::move(captured->second); p->bytes=p->data.size();
            send.posted+=p->bytes; send.captured.erase(captured);
            ep.localRequest(std::move(p));
            if (send.posted==send.descriptor.bytes) {
                const auto token=identity(TxKind,tickets_[index].generation,index);
                log("source_complete",c.id,token,0,send.descriptor.bytes,send.descriptor.invocation_id);
                tickets_[index].complete=true; ++sourceComplete_; --commandCount_;
                sends_.erase(found); checkWait();
            }
            cursor_=(index+1)%tickets_.size();
            break;
        }
        if (send.issued<send.descriptor.bytes && ep.local_.size()<ep.window_ && buffered_<ep.window_) {
            auto p=std::make_unique<Packet>();
            p->requestId=nextRequest(); p->destinationTile=ep.tile_;
            p->address=send.descriptor.source_address-ep.sourceBase()+send.issued;
            p->bytes=std::min<std::uint64_t>({ep.maxBytes_,ep.peers_[c.destination].maxBytes,send.descriptor.bytes-send.issued});
            reads_.emplace(p->requestId,Read{false,index,send.issued}); send.issued+=p->bytes;
            ++buffered_; maxBuffered_=std::max<std::uint64_t>(maxBuffered_,buffered_);
            ep.localRequest(std::move(p));
            cursor_=(index+1)%tickets_.size();
            break;
        }
    }
    checkWait();
    if (ep.cycleProfile_) {
        const auto now=ep.getCurrentSimTimeNano();
        ep.cycleProfile_.record(now,"state","net_commands",0,commandCount_);
        ep.cycleProfile_.record(now,"state","net_tickets",0,tickets_.size()-freeTickets_.size());
        ep.cycleProfile_.record(now,"state","net_packet_buffers",0,buffered_);
        ep.cycleProfile_.record(now,"state","net_control_packets",0,controls_.size());
    }
}

void NetworkEngine::queueControl(std::unique_ptr<NetworkSlotControl> control) {
    if (controls_.size()>=receiveSlots_.size()) endpoint_.fail("application control capacity exceeded");
    controls_.push_back(std::move(control));
}
bool NetworkEngine::sendControl() {
    if (controls_.empty()) return false;
    auto& ep=endpoint_; auto* control=controls_.front().get();
    const auto flit=ep.flitBits_/8;
    const auto wire=std::max<std::uint64_t>(2,(NetworkSlotControl::HeaderBytes+flit-1)/flit)*flit;
    if (!ep.network_->spaceToSend(0,wire*8)) { ++ep.networkStalls_; return true; }
    auto request=std::make_unique<MordredSpmEndpoint::Network::Request>(control->destination,ep.tile_,wire*8,true,true,control);
    request->vn=0; request->allow_adaptive=false;
    if (!ep.network_->send(request.get(),0)) { request->takePayload(); ++ep.networkStalls_; return true; }
    log("slot_credit_send",control->transferId,control->generation,control->slot);
    request.release(); controls_.front().release(); controls_.pop_front();
    ++controlSent; ++ep.packetsInNic_; ep.maxPendingNic_=std::max(ep.maxPendingNic_,ep.packetsInNic_);
    ep.wireSent_+=wire;
    return true;
}
void NetworkEngine::control(const NetworkSlotControl& control) {
    ++controlReceived;
    const auto found=byTransfer_.find(control.transferId);
    if (control.kind!=NetworkSlotControl::Credit || control.protocol!=2 || control.reserved ||
        control.destination!=endpoint_.tile_ || found==byTransfer_.end())
        endpoint_.fail("invalid application-slot credit envelope");
    auto& c=transfers_[found->second];
    if (c.source!=endpoint_.tile_ || !c.advertised || control.source!=c.destination ||
        control.slot>=c.remoteGeneration.size() || c.remoteAvailable[control.slot] ||
        control.generation!=c.remoteGeneration[control.slot]+1 || control.generation>GenerationLimit ||
        control.address || control.stride || control.capacity || control.slots)
        endpoint_.fail("stale, duplicate, or misrouted application-slot credit");
    c.remoteGeneration[control.slot]=control.generation;
    c.remoteAvailable[control.slot]=true; c.available.push_back(control.slot);
    log("slot_credit_received",c.id,control.generation,control.slot);
}

void NetworkEngine::admit(const Packet& p) {
    const auto found=byTransfer_.find(p.transferId);
    if (!p.write || p.metadata || p.messageReserved || found==byTransfer_.end())
        endpoint_.fail("malformed or unconfigured guest message packet");
    auto& c=transfers_[found->second];
    if (c.destination!=endpoint_.tile_ || p.destinationTile!=c.destination || p.sourceTile!=c.source ||
        p.messageSlot>=c.slots.size() || !p.messageBytes || p.messageBytes>c.capacity ||
        p.invocationId>INT64_MAX || !p.messageSequence || p.messageSequence>INT64_MAX ||
        p.messageSequence<c.nextReceive || p.messageOffset>p.messageBytes || p.bytes>p.messageBytes-p.messageOffset ||
        p.address!=c.address-SpmBase+p.messageSlot*c.capacity+p.messageOffset)
        endpoint_.fail("guest message does not match its deployed transfer");
    auto& slot=c.slots[p.messageSlot];
    if (slot.generation!=p.messageGeneration) endpoint_.fail("message used a stale application slot");
    if (slot.state==Free) {
        if (p.messageOffset) endpoint_.fail("first message packet did not start at zero");
        for (const auto& other:c.slots)
            if (other.state!=Free && other.sequence==p.messageSequence) endpoint_.fail("duplicate live message sequence");
        slot.state=Filling; slot.sequence=p.messageSequence; slot.bytes=p.messageBytes;
        slot.invocation=p.invocationId; slot.admitted=slot.committed=0;
    }
    if (slot.state!=Filling || slot.sequence!=p.messageSequence || slot.bytes!=p.messageBytes ||
        slot.invocation!=p.invocationId || slot.admitted!=p.messageOffset)
        endpoint_.fail("packet overwrote an owned slot or violated message chunk order");
    slot.admitted+=p.bytes;
    std::uint64_t occupied=0;
    for (const auto& transfer:transfers_) for (const auto& item:transfer.slots) occupied+=item.state!=Free;
    maxSlots_=std::max(maxSlots_,occupied);
    log("chunk_admit",c.id,p.messageSequence,p.messageOffset,p.bytes,p.invocationId);
}
void NetworkEngine::committed(const Packet& p) {
    const auto transfer=byTransfer_.at(p.transferId);
    auto& c=transfers_[transfer]; auto& slot=c.slots.at(p.messageSlot);
    if (slot.state!=Filling || slot.sequence!=p.messageSequence || slot.generation!=p.messageGeneration ||
        slot.invocation!=p.invocationId || p.bytes>slot.bytes-slot.committed) endpoint_.fail("invalid message SPM commit");
    slot.committed+=p.bytes;
    log("chunk_commit",c.id,p.messageSequence,p.messageOffset,p.bytes,p.invocationId);
    if (slot.committed==slot.bytes) {
        if (slot.admitted!=slot.bytes || !c.ready.emplace(slot.sequence,p.messageSlot).second)
            endpoint_.fail("invalid whole-message completion");
        slot.state=Ready;
        log("ready",c.id,slot.sequence,p.messageSlot,slot.bytes,slot.invocation);
        makeEligible(transfer); checkWait();
    }
}
bool NetworkEngine::idle() const {
    if (cpu_ || metadata_ || !eligible_.empty() || !sends_.empty() || !reads_.empty() || !controls_.empty() || buffered_ ||
        freeTickets_.size()!=tickets_.size()) return false;
    for (const auto& c:transfers_) {
        for (const auto& slot:c.slots) if (slot.state!=Free) return false;
        // Source completion and CPU retirement do not wait for release credits.
        // Simulation lifetime must still cover their eventual return, including
        // the interval after the final credit has left the destination NIC.
        if (std::find(c.remoteAvailable.begin(),c.remoteAvailable.end(),false)!=c.remoteAvailable.end()) return false;
    }
    return true;
}
void NetworkEngine::finish() {
    if (!idle() || submitted_!=sourceComplete_ || received_!=released_ ||
        controlSent!=released_ || controlReceived!=submitted_)
        endpoint_.fail("simulation ended with live guest message ownership or credits");
    trace_.flush();
    std::cout << "NETWORK_STATS {\"component\":" << std::quoted(endpoint_.getName())
        << ",\"configured_transfers\":" << transfers_.size() << ",\"receive_slots\":" << receiveSlots_.size()
        << ",\"submitted\":" << submitted_ << ",\"source_complete\":" << sourceComplete_
        << ",\"received\":" << received_ << ",\"released\":" << released_
        << ",\"would_block\":" << wouldBlock_ << ",\"descriptor_bytes\":" << descriptorBytes_
        << ",\"payload_bytes\":" << payloadBytes_ << ",\"max_commands\":" << maxCommands_
        << ",\"max_tickets\":" << maxTickets_ << ",\"max_packet_buffers\":" << maxBuffered_
        << ",\"max_occupied_slots\":" << maxSlots_ << ",\"receive_wait_cycles\":" << receiveWaitCycles_
        << ",\"send_wait_cycles\":" << sendWaitCycles_ << ",\"control_packets_sent\":" << controlSent
        << ",\"control_packets_received\":" << controlReceived << ",\"idle\":true}\n";
}
}
