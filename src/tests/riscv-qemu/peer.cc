#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

namespace TileComponents {
// Deliberately uses StandardMem, with no access to QEMU's shared backing file.
class RiscvPeer final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(RiscvPeer, "tilecomponents", "RiscvPeer",
        SST_ELI_ELEMENT_VERSION(1,0,0), "RISC-V scratchpad integration test peer", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"spm_request_bytes", "Configured SPM request/line size", "32"},
        {"observe_vector_store", "Poll the first vector output word while waiting for READY", "false"})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({"memory", "Independent SPM access", "SST::Interfaces::StandardMem"})
    RiscvPeer(SST::ComponentId_t id, SST::Params& p) : Component(id),
        requestBytes_(p.find<unsigned>("spm_request_bytes", 32)),
        observeVectorStore_(p.find<bool>("observe_vector_store", false)) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<RiscvPeer, &RiscvPeer::tick>(this));
        memory_ = loadUserSubComponent<Memory>("memory", SST::ComponentInfo::SHARE_NONE, clock_,
            new Memory::Handler<RiscvPeer, &RiscvPeer::response>(this));
        if (!memory_) fatal(CALL_INFO, -1, "RISC-V peer memory interface missing\n");
        if (const char* output = std::getenv("TILE_COMPONENT_OUTPUT")) {
            polls_.open(std::string(output) + "/peer-polls.csv");
            if (!polls_) fatal(CALL_INFO, -1, "RISC-V peer cannot open poll trace\n");
            polls_ << "index,cycle,value\n";
            if (observeVectorStore_) {
                vectorPolls_.open(std::string(output) + "/peer-vector-polls.csv");
                if (!vectorPolls_) fatal(CALL_INFO, -1, "RISC-V peer cannot open vector poll trace\n");
                vectorPolls_ << "index,cycle,value\n";
            }
        }
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void init(unsigned phase) override { memory_->init(phase); }
    void setup() override { memory_->setup(); }
    void finish() override {
        if (!done_ || !pending_.empty() || verifiedBytes_ != 40)
            fatal(CALL_INFO, -1, "RISC-V peer did not complete data verification\n");
        polls_.flush();
        if (observeVectorStore_) vectorPolls_.flush();
        std::cout << "RISCV_PEER_RESULT {\"passed\":true,\"verified_bytes\":" << verifiedBytes_
                  << ",\"end_cycle\":" << getCurrentSimTime(clock_) << "}\n";
    }
private:
    using Memory = SST::Interfaces::StandardMem;
    static constexpr std::uint64_t Base = 0x100000;
    enum class Poll { None, Ready, Vector };
    struct Pending { bool write; Poll poll; std::uint64_t pollIndex; std::vector<std::uint8_t> expected; };
    Memory* memory_ = nullptr;
    SST::TimeConverter clock_;
    unsigned requestBytes_, phase_ = 0;
    std::uint64_t verifiedBytes_ = 0;
    std::uint64_t nextPoll_ = 0, nextVectorPoll_ = 0;
    std::ofstream polls_, vectorPolls_;
    bool observeVectorStore_, pollVectorNext_ = false;
    bool ready_ = false, done_ = false;
    std::map<Memory::Request::id_t, Pending> pending_;
    static std::vector<std::uint8_t> bytes(std::uint64_t value, unsigned size) {
        std::vector<std::uint8_t> data(size);
        for (unsigned i = 0; i < size; ++i) data[i] = value >> (8 * i);
        return data;
    }
    void access(std::uint64_t address, const std::vector<std::uint8_t>& data, bool write, Poll poll = Poll::None) {
        for (unsigned offset = 0; offset < data.size();) {
            const unsigned n = std::min<unsigned>(data.size() - offset, requestBytes_ - (address + offset) % requestBytes_);
            std::vector<std::uint8_t> piece(data.begin() + offset, data.begin() + offset + n);
            Memory::Request* request = write ? static_cast<Memory::Request*>(new Memory::Write(address + offset, n, piece)) :
                                              static_cast<Memory::Request*>(new Memory::Read(address + offset, n));
            request->setNoncacheable();
            const auto pollIndex = poll == Poll::Ready ? nextPoll_++ :
                                   poll == Poll::Vector ? nextVectorPoll_++ : 0;
            pending_.emplace(request->getID(), Pending{write, poll, pollIndex, std::move(piece)});
            memory_->send(request); offset += n;
        }
    }
    bool tick(SST::Cycle_t) {
        if (getCurrentSimTime(clock_) > 10000000) fatal(CALL_INFO, -1, "RISC-V peer progress timeout\n");
        if (phase_ == 2 && !ready_) {
            // Keep the same line busy while CPU writes are in flight. These
            // queued reads must wait for the actual QEMU store, including the
            // interval between backend completion and CPU response delivery.
            while (pending_.size() < 4) {
                if (observeVectorStore_ && pollVectorNext_)
                    access(Base + 0x44, bytes(0x111, 4), false, Poll::Vector);
                else
                    access(Base + 0x40, bytes(0xc001c0de, 4), false, Poll::Ready);
                if (observeVectorStore_) pollVectorNext_ = !pollVectorNext_;
            }
            return false;
        }
        if (!pending_.empty()) return false;
        switch (phase_) {
        case 0:
            for (unsigned i = 0; i < 8; ++i) access(Base + 4 + 4*i, bytes(0x100 + 3*i, 4), true);
            access(Base + 0xa0, bytes(UINT64_C(0x0000806701100513), 8), true); // li a0,17; ret
            ++phase_; break;
        case 1:
            access(Base, bytes(0x1badb002, 4), true); ++phase_; break;
        case 2:
            for (unsigned i = 0; i < 8; ++i) access(Base + 0x44 + 4*i, bytes(0x111 + 3*i, 4), false);
            access(Base + 0x68, bytes(UINT64_C(0x1122334455667788), 8), false);
            ++phase_; break;
        case 3:
            access(Base + 0xa0, bytes(0x01d00513, 4), true); ++phase_; break; // li a0,29
        case 4:
            access(Base + 0x80, bytes(0x600d600d, 4), true); ++phase_; break;
        case 5:
            done_ = true; primaryComponentOKToEndSim(); return true;
        }
        return false;
    }
    void response(Memory::Request* request) {
        auto it = pending_.find(request->getID());
        if (it == pending_.end()) fatal(CALL_INFO, -1, "RISC-V peer received unmatched response\n");
        if (it->second.write) {
            if (!dynamic_cast<Memory::WriteResp*>(request)) fatal(CALL_INFO, -1, "RISC-V peer missing write response\n");
        } else {
            auto* read = dynamic_cast<Memory::ReadResp*>(request);
            if (!read || read->data.size() != it->second.expected.size())
                fatal(CALL_INFO, -1, "RISC-V peer invalid read response\n");
            if (it->second.poll != Poll::None) {
                if (it->second.poll == Poll::Ready)
                    ready_ |= read->data == it->second.expected;
                std::uint32_t value = 0;
                for (unsigned i = 0; i < read->data.size(); ++i) value |= std::uint32_t(read->data[i]) << (8*i);
                auto& trace = it->second.poll == Poll::Vector ? vectorPolls_ : polls_;
                trace << it->second.pollIndex << ',' << getCurrentSimTime(clock_) << ',' << value << '\n';
            }
            else {
                if (read->data != it->second.expected) fatal(CALL_INFO, -1, "RISC-V guest output mismatched in SPM\n");
                verifiedBytes_ += read->data.size();
            }
        }
        pending_.erase(it); delete request;
    }
};
}
