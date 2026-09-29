#include <sst/core/sst_config.h>
#include "spmEndpoint.h"
#include "../observations.h"
#include <sst/core/unitAlgebra.h>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace TileComponents {
MordredSpmEndpoint::MordredSpmEndpoint(SST::ComponentId_t id, SST::Params& p) : Component(id),
    tile_(p.find<std::uint32_t>("tile_id",0)), tiles_(p.find<std::uint32_t>("tile_count",4)),
    requestBytes_(p.find<std::uint32_t>("spm_request_bytes",32)), banks_(p.find<std::uint32_t>("spm_banks",4)),
    bankWidth_(p.find<std::uint32_t>("spm_bank_width",4)), window_(p.find<std::uint32_t>("request_window",4)),
    maxBytes_(p.find<std::uint32_t>("max_request_bytes",256)), memoryDepth_(p.find<std::uint32_t>("memory_queue_depth",8)),
    flitBits_(p.find<std::uint32_t>("flit_size_bits",128)),
    postedSlots_(p.find<std::uint32_t>("posted_receive_slots_per_source",16)),
    creditBatch_(p.find<std::uint32_t>("posted_credit_batch",4)),
    creditDelay_(p.find<std::uint32_t>("posted_credit_delay_cycles",4)),
    capacity_(p.find<std::uint64_t>("spm_capacity_bytes",2097152)) {
    cycleProfile_.open(getName());
    if (!tiles_ || tile_>=tiles_ || !requestBytes_ || !banks_ || !bankWidth_ || !window_ ||
        !maxBytes_ || !memoryDepth_ || !flitBits_ || flitBits_%8 || !capacity_ || !creditBatch_ || !creditDelay_ ||
        (flitBits_ && std::max<std::uint64_t>(2,
            ((Packet::HeaderBytes+std::uint64_t(maxBytes_))*8+flitBits_-1)/flitBits_)*flitBits_>INT32_MAX))
        fail("invalid tile, SPM, window, packet, or memory-depth parameter");
    std::vector<std::uint32_t> bankIds;
    if (p.contains("router_spm_banks")) p.find_array("router_spm_banks",bankIds);
    else for (auto bank=banks_>1 ? banks_-2 : 0; bank<banks_; ++bank) bankIds.push_back(bank);
    if (bankIds.empty()) fail("router_spm_banks must not be empty");
    allowedBanks_.assign(banks_,false);
    for (const auto bank : bankIds) {
        if (bank>=banks_ || allowedBanks_[bank]) fail("invalid or duplicate router SPM bank ID");
        allowedBanks_[bank]=true;
    }
    receiveReservations_.assign(tiles_,0);
    peers_.resize(tiles_); peers_[tile_].seen=true;
    postedReservations_.assign(tiles_,0); pendingCredits_.assign(tiles_,0);
    creditSince_.assign(tiles_,0); receivedPostedId_.assign(tiles_,0);
    postedSendSequence_.assign(tiles_,0); postedReceiveSequence_.assign(tiles_,0);
    postedReorder_.resize(tiles_);
    creditBatch_=std::min(creditBatch_,postedSlots_);
    clockName_=p.find<std::string>("clock","1GHz");
    clock_=registerClock(clockName_,new SST::Clock::Handler<MordredSpmEndpoint,&MordredSpmEndpoint::tick>(this));
    memory_=loadUserSubComponent<Memory>("memory",SST::ComponentInfo::SHARE_NONE,clock_,
        new Memory::Handler<MordredSpmEndpoint,&MordredSpmEndpoint::memoryResponse>(this));
    network_=loadUserSubComponent<Network>("networkIF",SST::ComponentInfo::SHARE_NONE,1);
    requests_=configureLink("requests",clock_,new SST::Event::Handler<MordredSpmEndpoint,&MordredSpmEndpoint::localRequest>(this));
    arrivals_=configureLink("arrivals",clock_);
    if (!memory_ || !network_) fail("memory and networkIF subcomponents are required");
    network_->setNotifyOnReceive(new Network::Handler<MordredSpmEndpoint,&MordredSpmEndpoint::receiveNotification>(this));
    network_->setNotifyOnSend(new Network::Handler<MordredSpmEndpoint,&MordredSpmEndpoint::sendNotification>(this));
    if (const auto directory=CycleProfile::traceDirectory(); !directory.empty()) {
        trace_.open(std::string(directory)+"/"+getName()+"-spm.csv");
        if (!trace_) fail("cannot open router SPM timing trace");
        trace_ << "event,cycle,tile,request_id,source,destination,address,bytes,write,status,memory_request_id,posted,arrival,transport_sequence\n";
    }
    // Registration alone does not hold an Exit reference. Acquire/release it
    // only on live-work transitions; an unmatched OKToEndSim would decrement
    // SST's global reference count before this endpoint has acquired a hold.
    registerAsPrimaryComponent();
}
void MordredSpmEndpoint::init(unsigned phase) {
    memory_->init(phase); network_->init(phase);
    if (network_->isNetworkInitialized() && !advertised_) {
        advertised_=true;
        for (std::uint32_t peer=0; peer<tiles_; ++peer) if (peer!=tile_) {
            auto* advert=new MordredSpmAdvertisement;
            advert->tile=tile_; advert->tiles=tiles_; advert->banks=banks_; advert->bankWidth=bankWidth_;
            advert->maxBytes=maxBytes_; advert->capacity=capacity_; advert->slots=postedSlots_;
            advert->arrivals=arrivals_!=nullptr;
            for (std::uint32_t bank=0; bank<banks_; ++bank) if (allowedBanks_[bank]) advert->allowedBanks.push_back(bank);
            auto* request=new Network::Request(peer,tile_,0,true,true,advert);
            request->vn=0; request->allow_adaptive=false; network_->sendUntimedData(request);
        }
    }
    while (auto* raw=network_->recvUntimedData()) {
        std::unique_ptr<Network::Request> request(raw);
        auto* advert=dynamic_cast<MordredSpmAdvertisement*>(request->inspectPayload());
        if (!advert || advert->protocol!=2 || advert->tile>=tiles_ || advert->tile==tile_ ||
            advert->tiles!=tiles_ || request->src!=advert->tile || request->dest!=tile_ || request->vn!=0 ||
            !advert->banks || !advert->bankWidth || !advert->maxBytes || !advert->capacity || advert->allowedBanks.empty())
            fail("invalid posted-write receiver advertisement");
        auto& peer=peers_[advert->tile];
        if (peer.seen) fail("duplicate receiver advertisement");
        peer.seen=true; peer.arrivals=advert->arrivals; peer.capacity=advert->capacity;
        peer.banks=advert->banks; peer.bankWidth=advert->bankWidth; peer.maxBytes=advert->maxBytes;
        peer.slots=advert->slots; peer.credits=advert->slots; peer.allowed.assign(advert->banks,false);
        for (auto bank:advert->allowedBanks) {
            if (bank>=peer.banks || peer.allowed[bank]) fail("invalid advertised router bank ID");
            peer.allowed[bank]=true;
        }
    }
}
void MordredSpmEndpoint::setup() {
    memory_->setup(); network_->setup();
    if (!network_->isNetworkInitialized() || network_->getEndpointID()!=tile_ ||
        network_->getLinkBW()!=SST::UnitAlgebra(clockName_)*SST::UnitAlgebra(std::to_string(flitBits_)+"b"))
        fail("network identity, clock, or flit width mismatch");
    for (const auto& peer:peers_) if (!peer.seen) fail("missing posted-write receiver advertisement");
    initialized_=true;
}
void MordredSpmEndpoint::complete(unsigned phase) { memory_->complete(phase); network_->complete(phase); }
void MordredSpmEndpoint::finish() {
    if (!idle() || holding_ || packetsInjected_!=requestsSent_+responsesSent_+creditPacketsSent_ ||
        postedAccepted_!=creditsReceived_ || postedCommitted_!=creditsSent_)
        fail("simulation ended with live router SPM transactions");
    memory_->finish(); network_->finish(); trace_.flush(); cycleProfile_.flush();
    std::cout << "MORDRED_SPM_STATS {\"component\":" << std::quoted(getName()) << ",\"tile_id\":" << tile_
        << ",\"idle\":true,\"bytes_read\":" << reads_ << ",\"bytes_written\":" << writes_
        << ",\"local_completed\":" << localCompleted_ << ",\"remote_completed\":" << remoteCompleted_
        << ",\"local_bank_completed\":" << localBankCompleted_ << ",\"local_bank_rejected\":" << localBankRejected_
        << ",\"local_rejected\":" << localRejected_ << ",\"remote_rejected\":" << remoteRejected_
        << ",\"requests_sent\":" << requestsSent_ << ",\"responses_sent\":" << responsesSent_
        << ",\"requests_received\":" << requestsReceived_ << ",\"responses_received\":" << responsesReceived_
        << ",\"wire_bytes_sent\":" << wireSent_ << ",\"wire_bytes_received\":" << wireReceived_
        << ",\"packets_injected\":" << packetsInjected_ << ",\"max_pending_nic_packets\":" << maxPendingNic_
        << ",\"memory_queue_stall_cycles\":" << memoryStalls_ << ",\"network_send_stall_cycles\":" << networkStalls_
        << ",\"max_pending_memory\":" << maxPending_ << ",\"max_incoming_requests\":" << maxIncoming_
        << ",\"max_local_requests\":" << maxLocal_
        << ",\"posted_accepted\":" << postedAccepted_ << ",\"posted_committed\":" << postedCommitted_
        << ",\"arrivals_sent\":" << arrivalsSent_
        << ",\"credit_packets_sent\":" << creditPacketsSent_ << ",\"credit_packets_received\":" << creditPacketsReceived_
        << ",\"credits_sent\":" << creditsSent_ << ",\"credits_received\":" << creditsReceived_
        << ",\"credit_wire_bytes_sent\":" << creditWireSent_ << ",\"credit_wire_bytes_received\":" << creditWireReceived_
        << ",\"posted_credit_stall_cycles\":" << postedCreditStalls_
        << ",\"max_posted_receive_reservations\":" << maxPostedReservations_
        << ",\"max_pending_credit_slots\":" << maxPendingCredits_
        << ",\"max_reserved_receiver_slots\":" << maxReservedCredits_
        << ",\"posted_protocol_version\":2,\"posted_reordered_packets\":" << reorderedPackets_
        << ",\"max_posted_reorder_pending_per_source\":" << maxReorderPending_
        << ",\"posted_reorder_wait_cycles\":" << reorderWaitCycles_ << "}\n";
}
std::uint32_t MordredSpmEndpoint::validate(const Packet& p,bool targetAccess) const {
    if (p.response || p.arrival || p.status!=Packet::Success || p.destinationTile>=tiles_ || !p.bytes || p.bytes>maxBytes_ ||
        (p.posted && (!p.write || p.destinationTile==p.sourceTile || !p.requestId)) ||
        (p.write ? p.data.size()!=p.bytes : !p.data.empty())) return Packet::Malformed;
    // Only the destination can decide which of its banks and addresses the
    // router may access. The origin checks the request envelope and credits.
    if (!targetAccess) return Packet::Success;
    if (p.address>=capacity_ || p.bytes>capacity_-p.address) return Packet::OutOfRange;
    const auto end=p.address+p.bytes;
    for (auto address=p.address; address<end;) {
        if (!allowedBanks_[(address/bankWidth_)%banks_]) return Packet::ForbiddenBank;
        address+=std::min<std::uint64_t>(end-address,bankWidth_-address%bankWidth_);
    }
    return Packet::Success;
}
std::uint32_t MordredSpmEndpoint::validatePosted(const Packet& p) const {
    if (!p.posted) return Packet::Success;
    const auto& peer=peers_[p.destinationTile];
    if (!peer.seen || !peer.arrivals || !peer.slots || p.bytes>peer.maxBytes || p.requestId<=lastPostedId_ ||
        p.transportSequence)
        return Packet::Malformed;
    if (p.address>=peer.capacity || p.bytes>peer.capacity-p.address) return Packet::OutOfRange;
    for (auto address=p.address,end=p.address+p.bytes; address<end;) {
        if (!peer.allowed[(address/peer.bankWidth)%peer.banks]) return Packet::ForbiddenBank;
        address+=std::min<std::uint64_t>(end-address,peer.bankWidth-address%peer.bankWidth);
    }
    return Packet::Success;
}
std::uint64_t MordredSpmEndpoint::wireBytes(const Packet& p) const {
    const auto flitBytes=flitBits_/8;
    return std::max<std::uint64_t>(2,(Packet::HeaderBytes+p.data.size()+flitBytes-1)/flitBytes)*flitBytes;
}
std::uint64_t MordredSpmEndpoint::creditWireBytes() const {
    const auto flitBytes=flitBits_/8;
    return std::max<std::uint64_t>(2,(MordredSpmCredit::HeaderBytes+flitBytes-1)/flitBytes)*flitBytes;
}
void MordredSpmEndpoint::trace(const char* event,const Packet& p,std::uint64_t address,
    std::uint64_t bytes,std::uint64_t memoryRequest) {
    if (!RecordComponentObservations) return;
    if (trace_) trace_ << event << ',' << getCurrentSimTime(clock_) << ',' << tile_ << ',' << p.requestId << ','
        << p.sourceTile << ',' << p.destinationTile << ',' << (bytes ? address : p.address) << ','
        << (bytes ? bytes : p.bytes) << ',' << p.write << ',' << p.status << ',' << memoryRequest << ','
        << p.posted << ',' << p.arrival << ',' << p.transportSequence << '\n';
}
[[noreturn]] void MordredSpmEndpoint::fail(const char* text) {
    fatal(CALL_INFO,-1,"MordredSpmEndpoint tile %u: %s\n",tile_,text); std::abort();
}
bool MordredSpmEndpoint::idle() const {
    for (std::uint32_t peer=0; peer<tiles_; ++peer)
        if (pendingCredits_[peer] || postedReservations_[peer] || peers_[peer].credits!=peers_[peer].slots) return false;
    return local_.empty() && incoming_.empty() && ready_.empty() && pending_.empty() && responses_.empty() &&
        outgoing_.empty() && !packetsInNic_ && (!initialized_ || !network_->requestToReceive(0));
}
void MordredSpmEndpoint::updateHold() {
    const bool live=!idle();
    if (live && !holding_) primaryComponentDoNotEndSim();
    else if (!live && holding_) primaryComponentOKToEndSim();
    holding_=live;
}
void MordredSpmEndpoint::returnLocal(std::unique_ptr<Packet> p,std::uint32_t status) {
    if (!requests_) fail("cannot deliver local response without a requests port");
    p->response=true; p->status=status;
    if (status!=Packet::Success || p->write) p->data.clear();
    trace("local_response",*p); requests_->send(p.release());
}
void MordredSpmEndpoint::localRequest(SST::Event* event) {
    auto* raw=dynamic_cast<Packet*>(event);
    if (!raw || !initialized_) fail("invalid local event or request before setup");
    std::unique_ptr<Packet> p(raw); p->sourceTile=tile_;
    auto status=validate(*p,false);
    if (!status) status=validatePosted(*p);
    if (!status && local_.count(p->requestId)) status=Packet::Malformed;
    if (!status && local_.size()==window_) status=Packet::Busy;
    if (status) { ++localRejected_; returnLocal(std::move(p),status); return; }
    if (p->destinationTile==tile_) {
        status=validate(*p);
        if (status) {
            ++localBankRejected_; p->status=status; trace("local_bank_reject",*p);
            returnLocal(std::move(p),status); return;
        }
    }
    trace("local_request",*p);
    if (p->posted) { lastPostedId_=p->requestId; trace("posted_queued",*p); }
    local_.emplace(p->requestId,Local{p->address,p->destinationTile,p->bytes,p->write,p->posted});
    maxLocal_=std::max<std::uint64_t>(maxLocal_,local_.size());
    if (p->destinationTile==tile_) {
        trace("local_bank_request",*p);
        auto job=std::make_shared<Job>(); job->packet=std::move(p); job->localBank=true;
        if (!job->packet->write) job->packet->data.resize(job->packet->bytes);
        ready_.push_back(std::move(job));
    } else outgoing_.push_back(std::move(p));
    updateHold();
}
bool MordredSpmEndpoint::receiveNotification(int vn) {
    if (!initialized_ || vn!=0) fail("invalid network receive notification");
    updateHold(); return true;
}
bool MordredSpmEndpoint::sendNotification(int vn) {
    // Mordred calls once per TAIL injection. The endpoint clock releases the
    // lifetime hold after the NIC's callback and transport send have returned.
    if (!initialized_ || vn!=0 || !packetsInNic_) fail("invalid network send notification");
    --packetsInNic_; ++packetsInjected_; return true;
}
void MordredSpmEndpoint::receivePackets() {
    while (network_->requestToReceive(0)) {
        std::unique_ptr<Network::Request> request(network_->recv(0));
        if (!request) fail("network receive indicator had no packet");
        if (auto* credit=dynamic_cast<MordredSpmCredit*>(request->inspectPayload())) {
            if (request->vn!=0 || !request->head || !request->tail ||
                request->size_in_bits!=creditWireBytes()*8 || credit->sourceTile>=tiles_ ||
                credit->destinationTile!=tile_ || request->src!=credit->sourceTile || request->dest!=tile_)
                fail("malformed posted credit network envelope");
            wireReceived_+=creditWireBytes(); creditWireReceived_+=creditWireBytes();
            receiveCredit(*credit); continue;
        }
        auto* raw=dynamic_cast<Packet*>(request->inspectPayload());
        if (!raw || raw->arrival || (raw->posted && raw->response) || request->vn!=0 || !request->head || !request->tail || raw->sourceTile>=tiles_ ||
            raw->destinationTile>=tiles_ || request->size_in_bits!=wireBytes(*raw)*8 ||
            request->src!=(raw->response ? raw->destinationTile : raw->sourceTile) || request->dest!=tile_ ||
            (raw->response ? raw->sourceTile : raw->destinationTile)!=tile_)
            fail("malformed network request envelope");
        wireReceived_+=wireBytes(*raw);
        std::unique_ptr<Packet> p(static_cast<Packet*>(request->takePayload()));
        if (p->response) receiveResponse(std::move(p)); else receiveRequest(std::move(p));
    }
}
void MordredSpmEndpoint::receiveRequest(std::unique_ptr<Packet> p) {
    const auto source=p->sourceTile;
    const Key key{source,p->requestId,p->posted};
    auto& reservations=p->posted ? postedReservations_[source] : receiveReservations_[source];
    const auto limit=p->posted ? postedSlots_ : window_;
    if (incoming_.count(key) || reservations>=limit)
        fail("source exceeded its receive window or repeated a live request");
    if (p->posted && (!arrivals_ || !p->transportSequence ||
        p->transportSequence<=postedReceiveSequence_[source] ||
        std::uint64_t(p->transportSequence)-postedReceiveSequence_[source]>postedSlots_ ||
        postedReorder_[source].count(p->transportSequence)))
        fail("guaranteed posted write has no consumer or invalid transport sequence");
    ++requestsReceived_; ++reservations;
    if (p->posted) maxPostedReservations_=std::max<std::uint64_t>(maxPostedReservations_,reservations);
    auto job=std::make_shared<Job>(); job->packet=std::move(p);
    incoming_.emplace(key,job);
    maxIncoming_=std::max<std::uint64_t>(maxIncoming_,incoming_.size());
    trace("request_recv",*job->packet);
    const auto status=validate(*job->packet);
    if (status) {
        if (job->packet->posted) fail("guaranteed posted write failed destination validation");
        ++remoteRejected_; completeRemote(job,status); return;
    }
    if (!job->packet->write) job->packet->data.resize(job->packet->bytes);
    if (job->packet->posted) {
        // Multiple VCs can deliver one source's packets out of order. Payload
        // storage stays inside the existing reserved receive slots while a
        // missing earlier packet is in flight. Admission to SPM retains send
        // order, including the ordinary per-fragment controller ordering.
        auto& held=postedReorder_[source];
        const auto sequence=job->packet->transportSequence;
        if (sequence!=postedReceiveSequence_[source]+1) {
            ++reorderedPackets_; trace("posted_sequence_wait",*job->packet);
        }
        held.emplace(sequence,std::move(job));
        while (postedReceiveSequence_[source]!=UINT32_MAX) {
            auto next=held.find(postedReceiveSequence_[source]+1);
            if (next==held.end()) break;
            const auto& ordered=*next->second->packet;
            if (ordered.requestId<=receivedPostedId_[source]) fail("posted caller identities are not ordered");
            receivedPostedId_[source]=ordered.requestId;
            ++postedReceiveSequence_[source]; trace("posted_admit",ordered);
            ready_.push_back(std::move(next->second)); held.erase(next);
        }
        maxReorderPending_=std::max<std::uint64_t>(maxReorderPending_,held.size());
    } else ready_.push_back(std::move(job));
}
void MordredSpmEndpoint::receiveResponse(std::unique_ptr<Packet> p) {
    const auto found=local_.find(p->requestId);
    if (found==local_.end()) fail("unsolicited or duplicate remote response");
    const auto& expected=found->second;
    const auto expectedBytes=p->status==Packet::Success && !p->write ? expected.bytes : 0;
    if (expected.posted || p->posted || p->arrival || expected.destination==tile_ || p->destinationTile!=expected.destination || p->address!=expected.address || p->bytes!=expected.bytes ||
        p->write!=expected.write || p->status>Packet::Busy || p->data.size()!=expectedBytes)
        fail("remote response identity or payload mismatch");
    ++responsesReceived_; ++localCompleted_; trace("response_recv",*p);
    local_.erase(found); const auto status=p->status; returnLocal(std::move(p),status);
}
void MordredSpmEndpoint::receiveCredit(const MordredSpmCredit& credit) {
    auto& peer=peers_[credit.sourceTile];
    if (credit.protocol!=1 || credit.sourceTile==tile_ || !credit.slots || !peer.seen ||
        credit.slots>peer.slots-peer.credits)
        fail("duplicate or excessive posted receiver credits");
    peer.credits+=credit.slots; ++creditPacketsReceived_; creditsReceived_+=credit.slots;
    Packet event; event.sourceTile=credit.sourceTile; event.destinationTile=tile_;
    event.bytes=credit.slots; event.posted=true; trace("credit_recv",event);
}
bool MordredSpmEndpoint::sendCredit() {
    const auto now=getCurrentSimTime(clock_);
    for (std::uint32_t offset=0; offset<tiles_; ++offset) {
        const auto peer=(nextCreditPeer_+offset)%tiles_;
        const auto count=pendingCredits_[peer];
        if (!count || (count<creditBatch_ && now-creditSince_[peer]<creditDelay_)) continue;
        const auto wire=creditWireBytes();
        if (!network_->spaceToSend(0,wire*8)) { ++networkStalls_; return true; }
        auto credit=std::make_unique<MordredSpmCredit>();
        credit->sourceTile=tile_; credit->destinationTile=peer; credit->slots=std::min(count,creditBatch_);
        auto request=std::make_unique<Network::Request>(peer,tile_,wire*8,true,true,credit.get());
        request->vn=0; request->allow_adaptive=false;
        if (!network_->send(request.get(),0)) { request->takePayload(); ++networkStalls_; return true; }
        const auto returned=credit->slots;
        request.release(); credit.release();
        pendingCredits_[peer]-=returned;
        ++creditPacketsSent_; creditsSent_+=returned; wireSent_+=wire; creditWireSent_+=wire;
        ++packetsInNic_; maxPendingNic_=std::max(maxPendingNic_,packetsInNic_);
        nextCreditPeer_=(peer+1)%tiles_;
        Packet event; event.sourceTile=tile_; event.destinationTile=peer;
        event.bytes=returned; event.posted=true; trace("credit_send",event);
        return true;
    }
    return false;
}
void MordredSpmEndpoint::notifyArrival(const Packet& packet) {
    if (!arrivals_) return;
    auto* event=new Packet(packet);
    event->response=true; event->arrival=true; event->status=Packet::Success; event->data.clear();
    ++arrivalsSent_; trace("arrival",*event); arrivals_->send(event);
}
void MordredSpmEndpoint::pumpMemory() {
    // One fragment per ready job before rotating: shared finite memory credits
    // do not let a large request permanently block a different source.
    while (!ready_.empty() && pending_.size()<memoryDepth_) {
        auto job=ready_.front(); ready_.pop_front();
        const auto& p=*job->packet;
        const auto offset=job->issued,address=p.address+offset;
        const auto bytes=std::min<std::uint64_t>(p.bytes-offset,requestBytes_-address%requestBytes_);
        Memory::Request* request;
        if (p.write) request=new Memory::Write(address,bytes,
            std::vector<std::uint8_t>(p.data.begin()+offset,p.data.begin()+offset+bytes));
        else request=new Memory::Read(address,bytes);
        request->setNoncacheable();
        pending_.emplace(request->getID(),Pending{job,offset,bytes}); job->issued+=bytes;
        if (job->issued<p.bytes) ready_.push_back(job);
        maxPending_=std::max<std::uint64_t>(maxPending_,pending_.size());
        trace(p.write ? "write_request" : "read_request",p,address,bytes,request->getID());
        memory_->send(request);
    }
}
void MordredSpmEndpoint::memoryResponse(Memory::Request* response) {
    const auto found=pending_.find(response->getID());
    if (found==pending_.end()) fail("unmatched StandardMem response");
    const auto pending=found->second; auto& p=*pending.job->packet;
    if (response->getFail() || (p.write ? !dynamic_cast<Memory::WriteResp*>(response) :
                                         !dynamic_cast<Memory::ReadResp*>(response)))
        fail("failed or incorrectly typed StandardMem response");
    if (!p.write) {
        const auto* read=static_cast<Memory::ReadResp*>(response);
        if (read->data.size()!=pending.bytes) fail("incorrect StandardMem read data length");
        std::copy(read->data.begin(),read->data.end(),p.data.begin()+pending.offset); reads_+=pending.bytes;
    } else writes_+=pending.bytes;
    trace(p.write ? "write_response" : "read_response",p,p.address+pending.offset,pending.bytes,response->getID());
    pending.job->completed+=pending.bytes; pending_.erase(found); delete response;
    if (pending.job->completed>p.bytes) fail("duplicate StandardMem completion bytes");
    if (pending.job->completed==p.bytes) {
        if (pending.job->localBank) {
            if (local_.erase(p.requestId)!=1) fail("local bank completion has no live request");
            ++localBankCompleted_; trace("local_bank_complete",p);
            returnLocal(std::move(pending.job->packet),Packet::Success);
        } else {
            ++remoteCompleted_;
            if (p.posted) { ++postedCommitted_; trace("posted_commit",p); }
            if (p.write) notifyArrival(p);
            if (p.posted) {
                const auto source=p.sourceTile;
                if (!postedReservations_[source] || incoming_.erase(Key{source,p.requestId,true})!=1)
                    fail("posted completion lost its reserved receiver slot");
                --postedReservations_[source];
                if (!pendingCredits_[source]) creditSince_[source]=getCurrentSimTime(clock_);
                ++pendingCredits_[source];
                if (pendingCredits_[source]>postedSlots_) fail("posted credit pending count exceeded reservations");
                maxPendingCredits_=std::max<std::uint64_t>(maxPendingCredits_,pendingCredits_[source]);
                trace("credit_pending",p);
            } else completeRemote(pending.job,Packet::Success);
        }
    }
}
void MordredSpmEndpoint::completeRemote(const std::shared_ptr<Job>& job,std::uint32_t status) {
    auto& p=*job->packet;
    if (p.response || p.posted) fail("remote request completed twice or posted write generated a commit ACK");
    p.response=true; p.status=status;
    if (status || p.write) p.data.clear();
    trace("response_ready",p); responses_.push_back(job);
}
void MordredSpmEndpoint::sendPacket() {
    if (sendCredit()) return;
    const bool response=!responses_.empty();
    auto* p=response ? responses_.front()->packet.get() : (outgoing_.empty() ? nullptr : outgoing_.front().get());
    if (!p) return;
    if (p->posted && !peers_[p->destinationTile].credits) { ++postedCreditStalls_; return; }
    const auto wire=wireBytes(*p);
    if (!network_->spaceToSend(0,wire*8)) { ++networkStalls_; return; }
    const auto destination=response ? p->sourceTile : p->destinationTile;
    if (p->posted) {
        if (postedSendSequence_[destination]==UINT32_MAX) fail("posted per-peer sequence exhausted");
        p->transportSequence=postedSendSequence_[destination]+1;
    }
    auto request=std::make_unique<Network::Request>(destination,tile_,wire*8,true,true,p);
    request->vn=0; request->allow_adaptive=false;
    if (p->posted) --peers_[p->destinationTile].credits;
    if (!network_->send(request.get(),0)) {
        if (p->posted) { ++peers_[p->destinationTile].credits; p->transportSequence=0; }
        request->takePayload(); ++networkStalls_; return;
    }
    request.release(); ++packetsInNic_; maxPendingNic_=std::max(maxPendingNic_,packetsInNic_); wireSent_+=wire;
    if (p->posted) ++postedSendSequence_[destination];
    if (response) {
        ++responsesSent_; trace("response_send",*p); --receiveReservations_[p->sourceTile];
        incoming_.erase(Key{p->sourceTile,p->requestId,false});
        responses_.front()->packet.release(); responses_.pop_front();
    } else {
        ++requestsSent_; trace("request_send",*p);
        if (p->posted) {
            const auto& peer=peers_[p->destinationTile];
            maxReservedCredits_=std::max<std::uint64_t>(maxReservedCredits_,peer.slots-peer.credits);
            ++postedAccepted_; trace("posted_accepted",*p);
            auto accepted=std::make_unique<Packet>(*p); accepted->data.clear();
            if (local_.erase(p->requestId)!=1) fail("posted NIC admission has no local request");
            returnLocal(std::move(accepted),Packet::Accepted);
        }
        outgoing_.front().release(); outgoing_.pop_front();
    }
}
bool MordredSpmEndpoint::tick(SST::Cycle_t) {
    if (!initialized_) fail("endpoint clock before setup");
    if (std::any_of(postedReorder_.begin(),postedReorder_.end(),[](const auto& held){return !held.empty();}))
        ++reorderWaitCycles_;
    receivePackets(); sendPacket();
    if (!ready_.empty() && pending_.size()==memoryDepth_) ++memoryStalls_;
    pumpMemory(); updateHold();
    if (cycleProfile_) {
        const auto now=getCurrentSimTimeNano();
        cycleProfile_.record(now,"state","local_requests",0,local_.size());
        cycleProfile_.record(now,"state","incoming_requests",0,incoming_.size());
        cycleProfile_.record(now,"state","ready_memory_jobs",0,ready_.size());
        cycleProfile_.record(now,"state","memory_fragments",0,pending_.size());
        cycleProfile_.record(now,"state","outgoing_packets",0,outgoing_.size());
        cycleProfile_.record(now,"state","response_packets",0,responses_.size());
        cycleProfile_.record(now,"state","nic_packets",0,packetsInNic_);
        cycleProfile_.record(now,"state","memory_queue_blocked",0,!ready_.empty() && pending_.size()==memoryDepth_);
        for (unsigned peer=0;peer<tiles_;++peer) if (peer!=tile_) {
            cycleProfile_.record(now,"state","send_credits",peer,peers_[peer].credits);
            cycleProfile_.record(now,"state","receive_reservations",peer,postedReservations_[peer]);
            cycleProfile_.record(now,"state","pending_credit_returns",peer,pendingCredits_[peer]);
            cycleProfile_.record(now,"state","reorder_packets",peer,postedReorder_[peer].size());
        }
    }
    return false;
}
}
