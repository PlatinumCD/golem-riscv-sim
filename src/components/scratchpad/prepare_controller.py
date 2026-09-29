"""Build-local, checked adaptation of SST's scratchpad controller.

Keep the upstream backing store and interfaces. Local writes use the existing
read-completion machinery instead of early acknowledgement. CPU-connected
systems order exact byte ranges independently of the transport request width.
No installed library or third_party source is modified.
"""
import re
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Unsupported upstream scratchpad source near {old[:100]!r}")
    return text.replace(old, new)


def prepare(upstream: Path, output: Path):
    header = (upstream / "scratchpad.h").read_text()
    source = (upstream / "scratchpad.cc").read_text()
    source = replace_once(source, '#include "scratchpad.h"',
                          '#include "scratchpad.h"\n#include "externalCommit.h"')
    header = replace_once(header, '#include <map>', '#include <map>\n#include "bankConnections.h"\n#include "../cycleProfile.h"')
    header = replace_once(header, '    Backend::Backing * backing_;', '''    Backend::Backing * backing_;
    TileComponents::BankConnections bankConnections_;
    TileComponents::CycleProfile cycleProfile_;
    void profileState();
    std::string externalWriteRequestor_;
    struct RangeHold {
        uint64_t bytes;
        SST::Event::id_type request;
        bool external;
        bool completed;
    };
    struct RangeWaiter {
        MemEvent* event;
        bool write;
    };
    std::map<Addr, RangeHold> activeRanges_;
    std::list<RangeWaiter> waitingRanges_;
    bool dispatchingRanges_ = false;
    static bool rangesOverlap(Addr left, uint64_t leftBytes, Addr right, uint64_t rightBytes);
    void dispatchRanges();
    void handleExternalCommit(SST::Event* event);''')
    header = replace_once(header, '    SST_ELI_DOCUMENT_PORTS(', '''    SST_ELI_DOCUMENT_PORTS(
            {"external_commit", "CPU shared-memory access committed; release its exact retained byte ranges", {"TileComponents.ExternalCommit"}},''')
    header = replace_once(header, '            {"backing_size_unit",', '''            {"external_write_requestor", "Exact requestor whose writes are timed here and committed by QEMU to shared mmap bytes after completion", ""},
            {"spm_banks", "Physical interleaved bank count", "8"},
            {"spm_bank_width", "Bytes per bank address stripe", "4"},
            {"cpu_spm_banks", "Physical bank IDs connected to the CPU", ""},
            {"router_spm_banks", "Physical bank IDs connected to the router", ""},
            {"cpu_requestor", "Exact CPU memory-interface requestor name", ""},
            {"router_requestor", "Exact router memory-interface requestor name", ""},
            {"memory_file", "Shared mmap backing file", ""},
            {"backing_size_unit",''')
    source = replace_once(source, '    // Assume no caching, may change during init', '''    cycleProfile_.open(getName());
    try { bankConnections_.configure(params); }
    catch (const std::exception& error) {
        out.fatal(CALL_INFO, -1, "Invalid SPM bank connections: %s\\n", error.what());
    }
    externalWriteRequestor_ = params.find<std::string>("external_write_requestor", "");
    if (!externalWriteRequestor_.empty() &&
        (backingType != "mmap" || params.find<std::string>("memory_file", "").empty()))
        out.fatal(CALL_INFO, -1, "External QEMU writes require shared mmap backing\\n");
    if (!externalWriteRequestor_.empty() &&
        !configureLink("external_commit", new SST::Event::Handler<Scratchpad,
            &Scratchpad::handleExternalCommit>(this)))
        out.fatal(CALL_INFO, -1, "External QEMU accesses require an external_commit link\\n");

    // Assume no caching, may change during init''')
    source = replace_once(source, '''    if (backing_) {
        backing_->set(event->getAddr(), event->getSize(), event->getPayload());
    }''', '''    // A pre-access QEMU stop carries no store value. Its actual store is
    // committed to these shared bytes only after the timing response arrives.
    if (backing_ && (externalWriteRequestor_.empty() ||
                    event->getRqstr() != externalWriteRequestor_)) {
        backing_->set(event->getAddr(), event->getSize(), event->getPayload());
    }''')
    header = replace_once(header, "    void setup() override;", "    void setup() override;\n    void finish() override;")
    source = replace_once(source, "void Scratchpad::setup() { }", '''void Scratchpad::setup() {
    if (caching_) out.fatal(CALL_INFO, -1, "This local scratchpad currently accepts uncached clients only\\n");
}
void Scratchpad::finish() {
    if (!activeRanges_.empty() || !waitingRanges_.empty())
        out.fatal(CALL_INFO, -1, "Simulation ended with outstanding ordered SPM byte ranges\\n");
    scratch_->finish();
}

bool Scratchpad::rangesOverlap(Addr left, uint64_t leftBytes, Addr right, uint64_t rightBytes) {
    // Admission has already bounded both ranges by the local scratchpad size.
    return left < right + rightBytes && right < left + leftBytes;
}

void Scratchpad::dispatchRanges() {
    if (dispatchingRanges_) return;
    dispatchingRanges_ = true;
    std::vector<std::pair<Addr, uint64_t>> olderWaiting;
    for (auto next = waitingRanges_.begin(); next != waitingRanges_.end();) {
        auto* request = next->event;
        const Addr address = request->getAddr();
        const uint64_t bytes = request->getSize();
        bool blocked = false;
        for (const auto& active : activeRanges_) {
            if (rangesOverlap(address, bytes, active.first, active.second.bytes)) {
                blocked = true;
                break;
            }
        }
        // A disjoint request can bypass older work, but must not pass an older
        // overlapping waiter merely because that waiter is blocked elsewhere.
        for (const auto& older : olderWaiting) {
            if (rangesOverlap(address, bytes, older.first, older.second)) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            olderWaiting.emplace_back(address, bytes);
            ++next;
            continue;
        }
        const bool write = next->write;
        next = waitingRanges_.erase(next);
        // Active ranges never overlap, so the exact starting address uniquely
        // identifies this request in the upstream completion/MSHR machinery.
        request->setBaseAddr(address);
        if (mshr_.count(address) || !activeRanges_.emplace(address, RangeHold{
                bytes, request->getID(), request->getRqstr() == externalWriteRequestor_, false}).second)
            out.fatal(CALL_INFO, -1, "Duplicate active SPM byte-range request\\n");
        if (write) handleScratchWrite(request);
        else handleScratchRead(request);
    }
    dispatchingRanges_ = false;
}

void Scratchpad::handleExternalCommit(SST::Event* event) {
    auto* commit = dynamic_cast<TileComponents::ExternalCommit*>(event);
    if (!commit || commit->ranges.empty() || externalWriteRequestor_.empty())
        out.fatal(CALL_INFO, -1, "External SPM commit event is invalid\\n");
    std::set<Addr> addresses;
    // Validate the complete acknowledgment before releasing any request. A
    // repeated address, partial range, early response, or peer request is invalid.
    for (const auto& range : commit->ranges) {
        const Addr address = range.first;
        const uint64_t bytes = range.second;
        const auto active = activeRanges_.find(address);
        const auto mshr = mshr_.find(address);
        if (!bytes || address >= scratchSize_ || bytes > scratchSize_ - address ||
            !addresses.insert(address).second || active == activeRanges_.end() ||
            active->second.bytes != bytes || !active->second.external || !active->second.completed ||
            mshr == mshr_.end() || mshr->second.size() != 1 ||
            mshr->second.front().id != active->second.request)
            out.fatal(CALL_INFO, -1, "External SPM commit does not match a completed held byte range\\n");
    }
    delete commit;
    for (const Addr address : addresses) {
        updateMSHR(address);
        activeRanges_.erase(address);
    }
    dispatchRanges();
}''')
    old_response = '''    if (outstandingEventList_.find(requestID)->second.request->getCmd() == Command::Put) {
        updatePut(requestID);
    } else { // Anything else - GetS, GetX, etc.
        finishRequest(requestID);
    }
    updateMSHR(baseAddr);
}'''
    source = replace_once(source, old_response, '''    const auto* request = outstandingEventList_.find(requestID)->second.request;
    const bool rangeOrdering = !externalWriteRequestor_.empty() || bankConnections_.hasRouter();
    const bool external = !externalWriteRequestor_.empty() &&
                          request->getRqstr() == externalWriteRequestor_;
    if (rangeOrdering) {
        auto active = activeRanges_.find(baseAddr);
        const auto mshr = mshr_.find(baseAddr);
        if (active == activeRanges_.end() || active->second.request != requestID ||
            active->second.external != external || active->second.completed ||
            mshr == mshr_.end() || mshr->second.size() != 1 || mshr->second.front().id != requestID)
            out.fatal(CALL_INFO, -1, "Invalid SPM byte-range completion\\n");
        active->second.completed = true;
    }
    if (request->getCmd() == Command::Put) {
        updatePut(requestID);
    } else { // Read, instruction fetch, or write timing completion.
        finishRequest(requestID);
    }
    // The external CPU snapshots/commits functional data after the response.
    // Retain its reads and writes until acknowledgment; ordinary peers have
    // already captured/applied their backing data and release on completion.
    if (!external) {
        updateMSHR(baseAddr);
        if (rangeOrdering) {
            activeRanges_.erase(baseAddr);
            dispatchRanges();
        }
    }
}''')
    begin = "void Scratchpad::handleScratchWrite(MemEvent * ev) {"
    end = "\n\n/*\n * Handle scratch Get"
    start = source.index(begin)
    stop = source.index(end, start)
    source = source[:start] + '''void Scratchpad::handleScratchWrite(MemEvent * ev) {
    if (!ev->queryFlag(MemEvent::F_NONCACHEABLE) || ev->queryFlag(MemEvent::F_NORESPONSE))
        out.fatal(CALL_INFO, -1, "Local writes require noncacheable completion requests\\n");
    const Addr base = ev->getBaseAddr();
    stat_ScratchWriteReceived->addData(1);
    MemEvent* response = ev->makeResponse();
    MemEvent* write = new MemEvent(getName(), ev->getAddr(), base, Command::PutM, ev->getPayload());
    write->copyMetadata(ev);
    responseIDMap_.emplace(write->getID(), ev->getID());
    responseIDAddrMap_.emplace(write->getID(), base);
    outstandingEventList_.emplace(ev->getID(), OutstandingEvent(ev, response));
    auto found = mshr_.find(base);
    if (found == mshr_.end()) {
        mshr_.emplace(base, std::list<MSHREntry>{MSHREntry(ev->getID(), Command::GetX, true, false)});
        doScratchWrite(write);
    } else {
        found->second.push_back(MSHREntry(ev->getID(), Command::GetX, write));
    }
}
''' + source[stop:]
    old = '''            doScratchWrite(entry->scratch);
            finishRequest(entry->id);
            mshr_.find(baseAddr)->second.pop_front();

            if (mem_h_is_debug_addr(baseAddr))
                dbg.debug(_L10_, "M: %-20" PRIu64 " %-20" PRIu64 " %-20s MSHR:Remove   0x%-16" PRIx64 "\\n",
                        getCurrentSimCycle(), timestamp_, getName().c_str(), baseAddr);
'''
    source = replace_once(source, old, '''            doScratchWrite(entry->scratch);
            break; // Retain the line until actual backend write completion.
''')
    for operation in ("Read", "Write"):
        marker = f"void Scratchpad::handle{operation}(MemEventBase * event) {{"
        source = replace_once(source, marker, marker + '''
    auto* local = static_cast<MemEvent*>(event);
    if (!local->getSize() || local->getAddr() >= scratchSize_ ||
        local->getSize() > scratchSize_ - local->getAddr() ||
        local->getAddr() % scratchLineSize_ + local->getSize() > scratchLineSize_)
        out.fatal(CALL_INFO, -1, "Request outside local scratchpad or crossing request line\\n");
    // Check the entire request before queuing it or touching functional bytes.
    const int denied = bankConnections_.deniedBank(local->getRqstr(), local->getAddr(), local->getSize());
    if (denied != -1)
        out.fatal(CALL_INFO, -1, "SPM bank connection denied: requestor=%s address=%llu bytes=%u bank=%d\\n",
                  local->getRqstr().c_str(), static_cast<unsigned long long>(local->getAddr()), local->getSize(), denied);
    // Keep transport fragmentation unchanged while ordering every client's
    // actual bytes. The original controller path remains for systems without
    // an external functional-memory owner.
    if (!externalWriteRequestor_.empty() || bankConnections_.hasRouter()) {
        if (!local->queryFlag(MemEvent::F_NONCACHEABLE) || local->queryFlag(MemEvent::F_NORESPONSE))
            out.fatal(CALL_INFO, -1, "Byte-range ordering requires uncached completion requests\\n");
        waitingRanges_.push_back(RangeWaiter{local, ''' + ("true" if operation == "Write" else "false") + '''});
        dispatchRanges();
        return;
    }
''')
    source = replace_once(source, '    Command cmd = ev->getCmd();', '''    Command cmd = ev->getCmd();
    if ((!externalWriteRequestor_.empty() || bankConnections_.hasRouter()) && cmd != Command::GetS &&
        cmd != Command::GetX && cmd != Command::Write)
        out.fatal(CALL_INFO, -1, "CPU-connected byte-range scratchpad accepts local uncached reads/writes only\\n");''')
    source = replace_once(source, 'bool Scratchpad::clock(Cycle_t cycle) {',
                          'bool Scratchpad::clock(Cycle_t cycle) {\n    profileState();')
    source = replace_once(source, 'void Scratchpad::finish() {',
                          'void Scratchpad::finish() {\n    cycleProfile_.flush();')
    source += '''
void SST::MemHierarchy::Scratchpad::profileState() {
    if (!cycleProfile_) return;
    const auto now = getCurrentSimTimeNano();
    cycleProfile_.record(now,"state","active_byte_ranges",0,activeRanges_.size());
    cycleProfile_.record(now,"state","waiting_byte_ranges",0,waitingRanges_.size());
    cycleProfile_.record(now,"state","outstanding_requests",0,outstandingEventList_.size());
    cycleProfile_.record(now,"state","response_queue",0,procMsgQueue_.size());
}
'''
    # Compile beside, not over, memHierarchy.Scratchpad. Retain upstream license.
    header = re.sub(r"\bScratchpad\b", "CompletionScratchpad", header)
    source = re.sub(r"\bScratchpad\b", "CompletionScratchpad", source)
    source = source.replace("Endpoint::CompletionScratchpad", "Endpoint::Scratchpad")
    header = replace_once(header,
        'CompletionScratchpad, "memHierarchy", "CompletionScratchpad",',
        'CompletionScratchpad, "tilecomponents", "Scratchpad",')
    output.mkdir(parents=True, exist_ok=True)
    (output / "scratchpad.h").write_text(header)
    (output / "scratchpad.cc").write_text(source)
