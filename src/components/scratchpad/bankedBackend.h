#pragma once
#include "bankConnections.h"
#include "../cycleProfile.h"
#include <sst/core/link.h>
#include <sst/elements/memHierarchy/membackend/memBackend.h>
#include <deque>
#include <fstream>
#include <map>

namespace TileComponents {

// SRAM timing only. memHierarchy's controller owns the functional bytes.
class BankedBackend final : public SST::MemHierarchy::SimpleMemBackend {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(BankedBackend, "tilecomponents", "BankedBackend",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Banked SRAM ports and channels",
        SST::MemHierarchy::SimpleMemBackend)
    SST_ELI_DOCUMENT_PARAMS(MEMBACKEND_ELI_PARAMS,
        {"spm_banks", "Interleaved bank count", NULL},
        {"spm_bank_width", "Bytes per bank port per cycle and address stripe", NULL},
        {"cpu_spm_banks", "Physical bank IDs connected to the CPU", ""},
        {"router_spm_banks", "Physical bank IDs connected to the router", ""},
        {"cpu_requestor", "Exact CPU memory-interface requestor name", ""},
        {"router_requestor", "Exact router memory-interface requestor name", ""},
        {"spm_read_ports_per_bank", "Read ports per bank", NULL},
        {"spm_write_ports_per_bank", "Write ports per bank", NULL},
        {"spm_channels", "Memory service channel count", NULL},
        {"spm_channel_width", "Bytes per channel per cycle", NULL},
        {"experimental_queue_entries", "Experimental backend queue depth; default retains fixed model", "64"})
    BankedBackend(SST::ComponentId_t, SST::Params&);
    bool issueRequest(ReqId, SST::MemHierarchy::Addr, bool, unsigned) override;
    bool isClocked() override { return false; }
    void finish() override;
private:
    class Tick final : public SST::Event {
    public:
        void serialize_order(SST::Core::Serialization::serializer& s) override {
            SST::Event::serialize_order(s);
        }
        ImplementSerializable(TileComponents::BankedBackend::Tick)
    };
    struct Request {
        ReqId id;
        std::uint64_t address, sequence, lastService = 0, ready = 0;
        unsigned bytes, issued = 0;
        bool write;
        std::string requestor;
    };
    void tick(SST::Event*);
    void record(const char*, std::uint64_t, const Request&, unsigned = 0,
                int = -1, int = -1, int = -1, const char* = "");
    SST::TimeConverter timebase_;
    SST::Link* ticks_;
    std::map<ReqId, Request> pending_;
    unsigned banks_, width_, readPorts_, writePorts_, channels_, channelWidth_, queueEntries_;
    BankConnections connections_;
    bool ticking_ = false, finished_ = false;
    std::uint64_t accepted_ = 0, completed_ = 0, retries_ = 0;
    std::uint64_t reads_ = 0, writes_ = 0, readBytes_ = 0, writeBytes_ = 0;
    std::uint64_t readConflicts_ = 0, writeConflicts_ = 0, channelStalls_ = 0;
    unsigned peakPending_ = 0, peakReads_ = 0, peakWrites_ = 0;
    std::ofstream trace_;
    CycleProfile cycleProfile_;
};
}
