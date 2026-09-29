#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace TileComponents {
// Controlled accesses from two real StandardMem interfaces. Neither interface
// bypasses the SPM controller or modifies the backing store directly.
class BankConnectionTest final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(BankConnectionTest, "tilecomponents", "BankConnectionTest",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Physical SPM bank connection checks", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"scenario", "Test scenario", "visibility"})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"cpu", "CPU-side accesses", "SST::Interfaces::StandardMem"},
        {"router", "Router-side accesses", "SST::Interfaces::StandardMem"},
        {"unknown", "Unbound access rejection probe", "SST::Interfaces::StandardMem"})
    BankConnectionTest(SST::ComponentId_t id, SST::Params& p) : Component(id),
        scenario_(p.find<std::string>("scenario", "visibility")) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<BankConnectionTest, &BankConnectionTest::tick>(this));
        for (const char* role : {"cpu", "router", "unknown"}) {
            auto* memory = loadUserSubComponent<Memory>(role, SST::ComponentInfo::SHARE_NONE, clock_,
                new Memory::Handler<BankConnectionTest, &BankConnectionTest::response>(this));
            if (!memory) fatal(CALL_INFO, -1, "missing bank test interface\n");
            interfaces_[role] = memory;
        }
        if (const char* path = std::getenv("TILE_COMPONENT_OUTPUT")) {
            trace_.open(std::string(path) + "/protocol.csv");
            trace_ << "event,cycle,label,role,address,bytes,write\n";
        }
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void init(unsigned phase) override { for (auto& entry : interfaces_) entry.second->init(phase); }
    void setup() override { for (auto& entry : interfaces_) entry.second->setup(); }
    void complete(unsigned phase) override { for (auto& entry : interfaces_) entry.second->complete(phase); }
    void finish() override {
        if (!done_ || !pending_.empty()) fatal(CALL_INFO, -1, "incomplete bank connection test\n");
        std::cout << "BANK_CONNECTION_RESULT {\"passed\":true,\"responses\":" << responses_ << "}\n";
        for (auto& entry : interfaces_) entry.second->finish();
    }
private:
    using Memory = SST::Interfaces::StandardMem;
    struct Request { std::string label, role; unsigned address, bytes; bool write; std::uint8_t value; };
    std::map<std::string, Memory*> interfaces_;
    std::map<Memory::Request::id_t, Request> pending_;
    SST::TimeConverter clock_;
    std::ofstream trace_;
    std::string scenario_;
    unsigned stage_ = 0, responses_ = 0;
    bool done_ = false;
    void record(const char* event, const Request& request) {
        trace_ << event << ',' << getCurrentSimTime(clock_) << ',' << request.label << ',' << request.role << ','
            << request.address << ',' << request.bytes << ',' << request.write << '\n';
        trace_.flush();
    }
    void issue(const char* label, const char* role, unsigned address, bool write,
               std::uint8_t value, unsigned bytes = 4) {
        Request request{label, role, address, bytes, write, value};
        Memory::Request* message = write ? static_cast<Memory::Request*>(
            new Memory::Write(address, bytes, std::vector<std::uint8_t>(bytes, value))) :
            static_cast<Memory::Request*>(new Memory::Read(address, bytes));
        message->setNoncacheable();
        pending_.emplace(message->getID(), request);
        record("issue", request);
        interfaces_.at(role)->send(message);
    }
    void response(Memory::Request* message) {
        auto found = pending_.find(message->getID());
        if (found == pending_.end() || message->getFail()) fatal(CALL_INFO, -1, "unexpected bank response\n");
        const auto request = found->second;
        auto* read = dynamic_cast<Memory::ReadResp*>(message);
        if ((!request.write && (!read || read->data != std::vector<std::uint8_t>(request.bytes, request.value))) ||
            (request.write && !dynamic_cast<Memory::WriteResp*>(message)))
            fatal(CALL_INFO, -1, "shared bank data visibility failure\n");
        record("response", request);
        pending_.erase(found); ++responses_; delete message;
    }
    bool tick(SST::Cycle_t) {
        if (getCurrentSimTime(clock_) > 500) fatal(CALL_INFO, -1, "bank test timeout\n");
        if (stage_ == 0) {
            stage_ = 1;
            if (scenario_.find("visibility") == 0) issue("cpu-write", "cpu", 8, true, 0x31);
            else if (scenario_ == "disjoint") {
                issue("cpu-write", "cpu", 0, true, 0x31); issue("router-write", "router", 4, true, 0x41);
            } else if (scenario_ == "shared-conflict" || scenario_ == "shared-two-ports") {
                issue("cpu-write", "cpu", 8, true, 0x31); issue("router-write", "router", 24, true, 0x41);
            } else if (scenario_ == "shared-read-write") {
                issue("cpu-read", "cpu", 8, false, 0x11); issue("router-write", "router", 24, true, 0x41);
            } else if (scenario_ == "cpu-forbidden") issue("denied", "cpu", 4, true, 0x99);
            else if (scenario_ == "router-forbidden") issue("denied", "router", 0, true, 0x99);
            else if (scenario_ == "cpu-span") issue("denied", "cpu", 8, true, 0x99, 8);
            else if (scenario_ == "router-span") issue("denied", "router", 8, true, 0x99, 8);
            else if (scenario_ == "unknown-requestor") issue("denied", "unknown", 8, true, 0x99);
            else fatal(CALL_INFO, -1, "unknown bank scenario\n");
            return false;
        }
        if (!pending_.empty()) return false;
        if (scenario_.find("visibility") == 0) {
            if (stage_ == 1) { issue("router-read", "router", 8, false, 0x31); ++stage_; return false; }
            if (stage_ == 2) { issue("router-write", "router", 8, true, 0x41); ++stage_; return false; }
            if (stage_ == 3) { issue("cpu-read", "cpu", 8, false, 0x41); ++stage_; return false; }
        }
        done_ = true; primaryComponentOKToEndSim(); return true;
    }
};
}
