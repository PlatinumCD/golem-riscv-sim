#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/link.h>
#include "../../components/riscv-qemu/externalCommit.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace TileComponents {
/* This fixture models the external CPU's timing/functional split explicitly.
 * The peer uses only StandardMem; direct shared-memory access is restricted to
 * the external CPU's delayed functional commit, as in the real QEMU bridge. */
class RangeOrderingProtocol final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(RangeOrderingProtocol, "tilecomponents", "RangeOrderingProtocol",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Scratchpad byte-range ordering regression", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"scenario", "Controlled request/commit sequence", "disjoint"},
        {"memory_file", "Shared backing file prepared by simulation.py", ""})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"external", "External CPU timing requests", "SST::Interfaces::StandardMem"},
        {"peer", "Independent ordinary memory requests", "SST::Interfaces::StandardMem"})
    SST_ELI_DOCUMENT_PORTS({"external_commit", "Functional CPU commit", {"TileComponents.ExternalCommit"}})
    RangeOrderingProtocol(SST::ComponentId_t id, SST::Params& params) : Component(id),
        scenario_(params.find<std::string>("scenario", "disjoint")),
        backingPath_(params.find<std::string>("memory_file", "")), expected_(Capacity, Initial) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<RangeOrderingProtocol, &RangeOrderingProtocol::tick>(this));
        external_ = loadUserSubComponent<Memory>("external", SST::ComponentInfo::SHARE_NONE, clock_,
            new Memory::Handler<RangeOrderingProtocol, &RangeOrderingProtocol::response>(this));
        peer_ = loadUserSubComponent<Memory>("peer", SST::ComponentInfo::SHARE_NONE, clock_,
            new Memory::Handler<RangeOrderingProtocol, &RangeOrderingProtocol::response>(this));
        commit_ = configureLink("external_commit", clock_);
        check(external_ && peer_ && commit_, "missing memory or commit interface");
        if (const char* directory = std::getenv("TILE_COMPONENT_OUTPUT")) {
            trace_.open(std::string(directory) + "/protocol.csv");
            check(bool(trace_), "cannot open protocol trace");
            trace_ << "event,cycle,label,address,bytes,write,external\n";
        }
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    ~RangeOrderingProtocol() override { if (backing_) munmap(backing_, Capacity); }
    void init(unsigned phase) override { external_->init(phase); peer_->init(phase); }
    void setup() override {
        external_->setup(); peer_->setup();
        const int fd = open(backingPath_.c_str(), O_RDWR);
        check(fd >= 0, "cannot open shared backing");
        void* mapped = mmap(nullptr, Capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        check(mapped != MAP_FAILED, "cannot map shared backing");
        backing_ = static_cast<std::uint8_t*>(mapped);
        check(std::equal(expected_.begin(), expected_.end(), backing_), "backing initialization mismatch");
    }
    void finish() override {
        check(done_ && pending_.empty(), "fixture ended with unfinished requests");
        check(std::equal(expected_.begin(), expected_.end(), backing_), "final shared-memory data mismatch");
        std::cout << "RANGE_ORDERING_RESULT {\"passed\":true,\"scenario\":\"" << scenario_
                  << "\",\"requests\":" << requests_.size() << ",\"checked_bytes\":" << Capacity
                  << ",\"end_cycle\":" << now() << "}\n";
    }
private:
    using Memory = SST::Interfaces::StandardMem;
    static constexpr unsigned Capacity = 4096, Base = 256, Hold = 20;
    static constexpr std::uint8_t Initial = 0x11, CpuValue = 0x31, PeerValue = 0x41;
    struct Request {
        std::string label;
        bool external, write, responded = false, committed = false;
        std::uint64_t address, responseCycle = 0;
        std::vector<std::uint8_t> data;
    };
    Memory *external_ = nullptr, *peer_ = nullptr;
    SST::Link* commit_ = nullptr;
    SST::TimeConverter clock_;
    std::string scenario_, backingPath_;
    std::uint8_t* backing_ = nullptr;
    std::vector<std::uint8_t> expected_;
    std::ofstream trace_;
    std::map<std::string, Request> requests_;
    std::map<Memory::Request::id_t, std::string> pending_;
    unsigned stage_ = 0;
    std::uint64_t launch_ = 0, stageCycle_ = 0;
    bool done_ = false;

    std::uint64_t now() const { return getCurrentSimTime(clock_); }
    void check(bool value, const char* message) {
        if (!value) fatal(CALL_INFO, -1, "range ordering fixture (%s): %s\n", scenario_.c_str(), message);
    }
    void trace(const char* event, const Request& request) {
        trace_ << event << ',' << now() << ',' << request.label << ',' << request.address << ','
               << request.data.size() << ',' << request.write << ',' << request.external << '\n';
        trace_.flush();
    }
    void issue(const std::string& label, bool external, bool write, unsigned offset, unsigned size,
               std::uint8_t value = Initial, unsigned prefix = 0, std::uint8_t prefixValue = Initial) {
        Request request{label, external, write, false, false, Base + offset, 0, std::vector<std::uint8_t>(size, value)};
        std::fill_n(request.data.begin(), prefix, prefixValue);
        check(requests_.emplace(label, request).second, "duplicate request label");
        Memory::Request* message = write ? static_cast<Memory::Request*>(new Memory::Write(request.address, size, request.data)) :
                                           static_cast<Memory::Request*>(new Memory::Read(request.address, size));
        message->setNoncacheable();
        pending_.emplace(message->getID(), label);
        trace("issue", request);
        (external ? external_ : peer_)->send(message);
    }
    bool ready(const std::string& label) const {
        auto it = requests_.find(label); return it != requests_.end() && it->second.responded;
    }
    void functional(Request& request) {
        check(request.external && request.responded && !request.committed, "invalid functional commit");
        if (request.write) {
            std::copy(request.data.begin(), request.data.end(), backing_ + request.address);
            std::copy(request.data.begin(), request.data.end(), expected_.begin() + request.address);
        } else {
            check(std::equal(request.data.begin(), request.data.end(), backing_ + request.address),
                  "external load observed a younger write before commit");
        }
        request.committed = true;
        trace("functional", request);
    }
    void release(std::initializer_list<const char*> labels) {
        auto* event = new ExternalCommit;
        for (const char* label : labels) {
            auto& request = requests_.at(label);
            functional(request);
            event->ranges.emplace_back(request.address, request.data.size());
            trace("commit", request);
        }
        commit_->send(event);
    }
    void invalidCommit() {
        auto* event = new ExternalCommit;
        const auto& request = requests_.at("A");
        if (scenario_ != "invalid-empty") event->ranges.emplace_back(request.address, request.data.size());
        if (scenario_ == "invalid-duplicate") event->ranges.push_back(event->ranges.front());
        if (scenario_ == "invalid-size") event->ranges.front().second /= 2;
        trace("invalid-commit", request);
        commit_->send(event);
        stage_ = 99;
    }
    void finishScenario() {
        check(pending_.empty(), "scenario still has outstanding requests");
        for (const auto& pair : requests_)
            check(!pair.second.external || pair.second.committed, "external response left uncommitted");
        check(std::equal(expected_.begin(), expected_.end(), backing_), "final backing differs from ordered oracle");
        done_ = true; primaryComponentOKToEndSim();
    }
    bool tick(SST::Cycle_t) {
        check(now() < 1000, "progress timeout (blocked or prematurely released range)");
        if (done_) return true;
        const bool invalid = scenario_.find("invalid-") == 0;
        if (!stage_) {
            launch_ = now(); stage_ = 1;
            if (scenario_ == "disjoint") {
                issue("A", true, false, 0, 16); issue("B", true, true, 16, 16, CpuValue);
            } else if (scenario_ == "disjoint-peer") {
                issue("A", true, true, 0, 16, CpuValue); issue("B", false, true, 16, 16, PeerValue);
            } else if (scenario_ == "wide-separate" || scenario_ == "wide-batch") {
                issue("A", true, true, 0, 32, CpuValue); issue("B", true, true, 32, 32, CpuValue + 1);
            } else if (scenario_ == "older-waiter" || invalid) {
                issue("A", scenario_ != "invalid-foreign", false, 0, scenario_ == "invalid-premature" ? 32 : 16);
            } else {
                issue("A", true, scenario_.find("war-") != 0, 0, 16,
                      scenario_.find("war-") == 0 ? Initial : CpuValue);
            }
            return false;
        }
        if (scenario_ == "invalid-premature" && stage_ == 1 && now() >= launch_ + 5) {
            check(!ready("A"), "premature test reached an already-completed request");
            invalidCommit(); return false;
        }
        if (stage_ == 99) return false; // Invalid commits must terminate in the controller.
        if (scenario_ == "disjoint" || scenario_ == "disjoint-peer") {
            if (stage_ == 1 && ready("A") && ready("B")) { stage_ = 2; stageCycle_ = now(); }
            if (stage_ == 2 && now() >= stageCycle_ + Hold) {
                if (scenario_ == "disjoint") release({"A", "B"}); else release({"A"});
                issue("verify", false, false, 16, 16, scenario_ == "disjoint" ? CpuValue : PeerValue);
                stage_ = 3;
            }
            if (stage_ == 3 && ready("verify")) finishScenario();
            return done_;
        }
        if (scenario_ == "wide-separate" || scenario_ == "wide-batch") {
            if (stage_ == 1 && ready("A") && ready("B")) {
                issue("P", false, false, 0, 32, CpuValue);
                issue("Q", false, false, 32, 32, CpuValue + 1);
                stage_ = 2; stageCycle_ = now();
            }
            if (stage_ == 2) {
                check(!ready("P") && !ready("Q"), "wide-fragment peer read escaped before commit");
                if (now() >= stageCycle_ + Hold) {
                    if (scenario_ == "wide-batch") { release({"A", "B"}); stage_ = 4; }
                    else { release({"A"}); stage_ = 3; stageCycle_ = now(); }
                }
            }
            if (stage_ == 3) {
                check(!ready("Q"), "committing one fragment released the other fragment");
                if (now() >= stageCycle_ + Hold) {
                    check(ready("P"), "first fragment remained blocked after its exact commit");
                    release({"B"}); stage_ = 4;
                }
            }
            if (stage_ == 4 && ready("P") && ready("Q")) finishScenario();
            return done_;
        }
        if (stage_ == 1 && ready("A")) {
            if (scenario_ == "invalid-foreign") { invalidCommit(); return false; }
            if (scenario_ == "older-waiter" || invalid) issue("P", false, true, scenario_ == "older-waiter" ? 8 : 0, 16, PeerValue);
            else {
                const bool partial = scenario_.find("partial") != std::string::npos;
                const bool read = scenario_.find("raw-") == 0;
                issue("P", false, !read, partial ? 8 : 0, 16, read ? (partial ? Initial : CpuValue) : PeerValue,
                      read && partial ? 8 : 0, CpuValue);
            }
            stage_ = 2; stageCycle_ = now();
        }
        if (stage_ == 2) {
            check(!ready("P"), "overlapping peer escaped between response and functional commit");
            if (scenario_ == "older-waiter" && now() == stageCycle_ + 2)
                issue("B", true, false, 16, 16, Initial, 8, PeerValue);
            if (scenario_ == "older-waiter") check(!ready("B"), "younger disjoint request bypassed older overlapping waiter");
            if (now() >= stageCycle_ + Hold) {
                if (invalid) invalidCommit();
                else { release({"A"}); stage_ = 3; }
            }
        }
        if (stage_ == 3 && ready("P")) {
            if (scenario_ == "older-waiter") {
                if (ready("B")) { release({"B"}); stage_ = 4; stageCycle_ = now(); }
            } else finishScenario();
        }
        if (scenario_ == "older-waiter" && stage_ == 4 && now() >= stageCycle_ + 2) finishScenario();
        return done_;
    }
    void response(Memory::Request* message) {
        auto found = pending_.find(message->getID());
        check(found != pending_.end(), "response has no outstanding request");
        auto& request = requests_.at(found->second);
        check(!request.responded, "duplicate response");
        if (request.write) {
            check(dynamic_cast<Memory::WriteResp*>(message) != nullptr, "wrong write response type");
            if (!request.external) std::copy(request.data.begin(), request.data.end(), expected_.begin() + request.address);
        } else {
            auto* read = dynamic_cast<Memory::ReadResp*>(message);
            check(read && read->data == request.data, "read response violates ordered data oracle");
        }
        request.responded = true; request.responseCycle = now();
        trace("response", request);
        pending_.erase(found); delete message;
    }
};
}
