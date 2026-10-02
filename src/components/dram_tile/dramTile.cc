#include <sst/core/sst_config.h>
#include "dramTile.h"
#include <algorithm>
#include <iomanip>
#include <iostream>

namespace TileComponents {
DramTile::DramTile(SST::ComponentId_t id, SST::Params& p) : MordredSpmEndpoint(id,p),
    base_(p.find<std::uint64_t>("dram_base",UINT64_C(0x100000000))),
    capacity_(p.find<std::uint64_t>("dram_capacity_bytes",UINT64_C(67108864))),
    requestBytes_(p.find<std::uint32_t>("dram_request_bytes",64)) {
    const auto spmEnd=UINT64_C(0x90000000)+p.find<std::uint64_t>("spm_capacity_bytes",2097152);
    if (!capacity_ || base_>INT64_MAX || capacity_>std::uint64_t(INT64_MAX)-base_ ||
        !requestBytes_ || requestBytes_>4096 || (requestBytes_&(requestBytes_-1)) ||
        base_%requestBytes_ || capacity_%requestBytes_ ||
        (base_<spmEnd && UINT64_C(0x90000000)<base_+capacity_))
        fail("invalid or overlapping DRAM payload address region/request size");
    dram_=loadUserSubComponent<Memory>("dram_memory",SST::ComponentInfo::SHARE_NONE,endpointClock(),
        new Memory::Handler<DramTile,&DramTile::response>(this));
    if (!dram_) fail("DRAM Tile requires a dram_memory StandardMem interface");
    if (const auto directory=CycleProfile::traceDirectory(); !directory.empty()) {
        trace_.open(directory+"/"+getName()+"-dram.csv");
        if (!trace_) fail("cannot open DRAM read trace");
        trace_ << "event,cycle,request_id,address,bytes,pending\n";
    }
    profile_.open(getName()+".dram");
}
void DramTile::init(unsigned phase) { dram_->init(phase); MordredSpmEndpoint::init(phase); }
void DramTile::setup() { dram_->setup(); MordredSpmEndpoint::setup(); }
void DramTile::complete(unsigned phase) { dram_->complete(phase); MordredSpmEndpoint::complete(phase); }
int DramTile::sourceRange(std::uint64_t address, std::uint64_t bytes) const {
    return !bytes || address<base_ || address-base_>=capacity_ || bytes>capacity_-(address-base_)
        ? GOLEM_NET_RANGE : 0;
}
std::uint32_t DramTile::memoryBoundary(const Packet& packet) const {
    return !packet.write && !packet.metadata ? requestBytes_ : MordredSpmEndpoint::memoryBoundary(packet);
}
void DramTile::record(const char* event, Memory::Request::id_t id, const Read& read) {
    const auto now=getCurrentSimTime(endpointClock());
    if (trace_) trace_ << event << ',' << now << ',' << id << ',' << base_+read.address
        << ',' << read.bytes << ',' << reads_.size() << '\n';
    profile_.record(now,"state","dram_pending_reads",0,reads_.size());
    profile_.record(now,event,"dram_read",0,read.bytes,id,base_+read.address);
}
void DramTile::sendMemory(const Packet& packet, Memory::Request* request) {
    if (packet.write || packet.metadata) { MordredSpmEndpoint::sendMemory(packet,request); return; }
    const auto* read=dynamic_cast<Memory::Read*>(request);
    if (!read) fail("DRAM payload interface only accepts reads");
    const auto now=getCurrentSimTime(endpointClock());
    if (reads_.empty()) busyStart_=now;
    Read pending{read->pAddr,read->size,now};
    if (!reads_.emplace(request->getID(),pending).second) fail("duplicate DRAM read ID");
    ++requests_; peak_=std::max<std::uint64_t>(peak_,reads_.size());
    record("issue",request->getID(),pending);
    dram_->send(request);
}
void DramTile::response(Memory::Request* response) {
    const auto found=reads_.find(response->getID());
    if (found==reads_.end()) fail("unmatched DRAM response");
    const auto pending=found->second;
    reads_.erase(found);
    const auto now=getCurrentSimTime(endpointClock());
    bytes_+=pending.bytes; latency_+=now-pending.cycle;
    if (reads_.empty()) busy_+=now-busyStart_;
    record("ready",response->getID(),pending);
    // Common NIU checks response type, data length and status, then assembles
    // the bounded packet buffers. Send completion retains its existing meaning.
    memoryResponse(response);
}
void DramTile::finish() {
    if (!reads_.empty()) fail("simulation ended with live DRAM payload reads");
    dram_->finish(); trace_.flush(); profile_.flush();
    MordredSpmEndpoint::finish();
    std::cout << "DRAM_TILE_STATS {\"component\":" << std::quoted(getName())
        << ",\"idle\":true,\"requests\":" << requests_ << ",\"bytes_read\":" << bytes_
        << ",\"max_pending_reads\":" << peak_ << ",\"read_latency_cycles_sum\":" << latency_
        << ",\"busy_cycles\":" << busy_ << "}\n";
}
}
