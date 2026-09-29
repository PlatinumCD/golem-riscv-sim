#include <sst/core/sst_config.h>
#include "bankedBackend.h"
#include "../fixed.h"
#include "../observations.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <tuple>

namespace TileComponents {
BankedBackend::BankedBackend(SST::ComponentId_t id, SST::Params& p)
    : SimpleMemBackend(id, p),
      banks_(p.find<unsigned>("spm_banks")), width_(p.find<unsigned>("spm_bank_width")),
      readPorts_(p.find<unsigned>("spm_read_ports_per_bank")),
      writePorts_(p.find<unsigned>("spm_write_ports_per_bank")),
      channels_(p.find<unsigned>("spm_channels")),
      channelWidth_(p.find<unsigned>("spm_channel_width")),
      queueEntries_(p.find<unsigned>("experimental_queue_entries", BackendQueueEntries)) {
    cycleProfile_.open(getName());
    if (!banks_ || !width_ || !readPorts_ || !writePorts_ || !channels_ || !channelWidth_ || !queueEntries_ ||
        !m_memSize || m_reqWidth < 4 || m_reqWidth > m_memSize ||
        (m_reqWidth & (m_reqWidth - 1)))
        output->fatal(CALL_INFO, -1, "invalid scratchpad configuration\n");
    try { connections_.configure(p); }
    catch (const std::exception& error) {
        output->fatal(CALL_INFO, -1, "Invalid SPM bank connections: %s\n", error.what());
    }
    timebase_ = getTimeConverter(Clock);
    ticks_ = configureSelfLink("service", Clock,
        new SST::Event::Handler<BankedBackend, &BankedBackend::tick>(this));
    if (const auto dir = CycleProfile::traceDirectory(); !dir.empty()) {
        trace_.open(std::string(dir) + "/" + getName() + ".csv");
        if (!trace_) output->fatal(CALL_INFO, -1, "cannot open scratchpad trace\n");
        trace_ << "event,cycle,id,requestor,address,bytes,write,bank,port,channel,pool,"
                  "read_bank_conflicts,write_bank_conflicts,channel_stall_request_cycles,queue_retries\n";
    }
}

bool BankedBackend::issueRequest(ReqId id, SST::MemHierarchy::Addr address,
                               bool write, unsigned bytes) {
    if (!bytes || bytes > m_reqWidth || address >= m_memSize || bytes > m_memSize - address ||
        address % m_reqWidth + bytes > m_reqWidth)
        output->fatal(CALL_INFO, -1, "scratchpad request outside supported range\n");
    const auto requestor = getRequestor(id);
    const int denied = connections_.deniedBank(requestor, address, bytes);
    if (denied != -1)
        output->fatal(CALL_INFO, -1, "SPM bank connection denied: requestor=%s address=%llu bytes=%u bank=%d\n",
                      requestor.c_str(), static_cast<unsigned long long>(address), bytes, denied);
    if (pending_.count(id)) output->fatal(CALL_INFO, -1, "duplicate scratchpad request\n");
    if (pending_.size() == queueEntries_) {
        cycleProfile_.record(getCurrentSimTime(timebase_),"event","queue_full",0,bytes,id);
        ++retries_; return false;
    }
    Request request{id, address, accepted_++, 0, 0, bytes, 0, write, requestor};
    pending_.emplace(id, request);
    cycleProfile_.record(getCurrentSimTime(timebase_),"state","pending_requests",0,pending_.size());
    peakPending_ = std::max<unsigned>(peakPending_, pending_.size());
    (write ? writeBytes_ : readBytes_) += bytes;
    record("accepted", getCurrentSimTime(timebase_), request, bytes);
    if (!ticking_) { ticking_ = true; ticks_->send(1, new Tick); }
    return true;
}

void BankedBackend::tick(SST::Event* event) {
    delete event;
    const auto now = getCurrentSimTime(timebase_);
    std::vector<ReqId> complete;
    for (const auto& [id, r] : pending_)
        if (r.issued == r.bytes && r.ready <= now) complete.push_back(id);
    for (auto id : complete) {
        record("completed", now, pending_.at(id), pending_.at(id).bytes);
        pending_.erase(id); ++completed_; handleMemResponse(id);
    }
    std::vector<Request*> order;
    for (auto& [id, r] : pending_) if (r.issued < r.bytes) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
        return std::tie(a->lastService, a->sequence) < std::tie(b->lastService, b->sequence);
    });
    std::vector<unsigned> reads(banks_), writes(banks_);
    unsigned used = 0, totalReads = 0, totalWrites = 0;
    for (auto* r : order) {
        if (used == channels_) {
            cycleProfile_.record(now,"event","channel_wait",0,r->bytes-r->issued,r->id);
            ++channelStalls_; continue;
        }
        unsigned remaining = std::min(channelWidth_, r->bytes - r->issued);
        bool serviced = false;
        while (remaining) {
            const auto address = r->address + r->issued;
            const unsigned bank = (address / width_) % banks_;
            auto& port = r->write ? writes[bank] : reads[bank];
            if (port == (r->write ? writePorts_ : readPorts_)) {
                cycleProfile_.record(now,"event",r->write ? "write_port_wait" : "read_port_wait",
                                    bank,r->bytes-r->issued,r->id);
                ++(r->write ? writeConflicts_ : readConflicts_); break;
            }
            const unsigned bytes = std::min<unsigned>(remaining, width_ - address % width_);
            record("service", now, *r, bytes, bank, port, used,
                   "shared");
            ++port; ++(r->write ? totalWrites : totalReads);
            ++(r->write ? writes_ : reads_);
            r->issued += bytes; remaining -= bytes; serviced = true;
        }
        if (serviced) { ++used; r->lastService = now; r->ready = now + BankLatency; }
    }
    peakReads_ = std::max(peakReads_, totalReads);
    peakWrites_ = std::max(peakWrites_, totalWrites);
    cycleProfile_.record(now,"state","pending_requests",0,pending_.size());
    if (cycleProfile_) {
        unsigned waiting=0;
        for (const auto& entry:pending_) waiting += entry.second.issued < entry.second.bytes;
        cycleProfile_.record(now,"state","requests_awaiting_service",0,waiting);
        cycleProfile_.record(now,"state","requests_awaiting_response",0,pending_.size()-waiting);
    }
    if (!pending_.empty()) ticks_->send(1, new Tick);
    else ticking_ = false;
}

void BankedBackend::record(const char* kind, std::uint64_t cycle, const Request& r,
                          unsigned bytes, int bank, int port, int channel, const char* pool) {
    if (!RecordComponentObservations) return;
    if (trace_) trace_ << kind << ',' << cycle << ',' << r.id << ',' << r.requestor << ','
        << r.address + (bank >= 0 ? r.issued : 0) << ',' << bytes << ',' << r.write << ','
        << bank << ',' << port << ',' << channel << ',' << pool << ','
        << readConflicts_ << ',' << writeConflicts_ << ',' << channelStalls_ << ',' << retries_ << '\n';
}

void BankedBackend::finish() {
    cycleProfile_.flush();
    if (finished_) return;
    finished_ = true;
    if (trace_) trace_.flush();
    if (!pending_.empty()) output->fatal(CALL_INFO, -1, "scratchpad finished with live requests\n");
    std::cout << "SPM_STATS {\"component\":" << std::quoted(getName())
        << ",\"accepted\":" << accepted_ << ",\"completed\":" << completed_
        << ",\"queue_retries\":" << retries_ << ",\"peak_pending\":" << peakPending_
        << ",\"read_bytes\":" << readBytes_ << ",\"write_bytes\":" << writeBytes_
        << ",\"read_service_beats\":" << reads_ << ",\"write_service_beats\":" << writes_
        << ",\"read_bank_conflicts\":" << readConflicts_ << ",\"write_bank_conflicts\":" << writeConflicts_
        << ",\"channel_stall_request_cycles\":" << channelStalls_
        << ",\"max_reads_serviced_same_cycle\":" << peakReads_
        << ",\"max_writes_serviced_same_cycle\":" << peakWrites_ << "}\n";
}
}
