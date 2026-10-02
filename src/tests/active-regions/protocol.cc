#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "../../components/analog-arrays/commands.h"
#include <cstring>
#include <iostream>
#include <map>
#include <memory>

namespace TileComponents {
class ActiveRegionProtocol final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(ActiveRegionProtocol, "tilecomponents", "ActiveRegionProtocol",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Active analog extent and retirement regression", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"pipeline", "Use two output slots", "true"})
    SST_ELI_DOCUMENT_PORTS({"commands", "Array commands", {"TileComponents.ArrayCommand"}})
    ActiveRegionProtocol(SST::ComponentId_t id, SST::Params& p) : Component(id) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<ActiveRegionProtocol, &ActiveRegionProtocol::tick>(this));
        commands_ = configureLink("commands", "1ns",
            new SST::Event::Handler<ActiveRegionProtocol, &ActiveRegionProtocol::response>(this));
        check(commands_ != nullptr, "missing command link");
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
        const auto cfg = [](unsigned r, unsigned c, bool error = false) {
            return Step{Operation::Configure, (std::uint64_t(r) << 32) | c, 0, {}, {}, error};
        };
        const auto input = [](Operation op, unsigned off, std::initializer_list<float> values) {
            return Step{op, off, unsigned(values.size()), encode(values)};
        };
        const auto store = [](unsigned off, std::initializer_list<float> values) {
            return Step{Operation::Store, off, unsigned(values.size()), {}, encode(values)};
        };
        const Step execute{Operation::Execute}, badExecute{Operation::Execute, 0, 0, {}, {}, true};
        groups_ = {{cfg(0,3,true)}, {cfg(2,0,true)}, {cfg(5,3,true)}, {cfg(2,6,true)}};
        auto badSlot = cfg(2,3,true); badSlot.array = 1; groups_.push_back({badSlot});
        auto deferred = cfg(2,3,true); deferred.deferred = true; groups_.push_back({deferred});
        auto malformed = cfg(2,3,true); malformed.count = 1; groups_.push_back({malformed});
        groups_.push_back({cfg(4,5)});
        Step full{Operation::Program, 0, 20}; full.payload = encode(std::vector<float>(20,10));
        groups_.push_back({full});
        groups_.push_back({input(Operation::Load,0,{1,1,1,1,1})});
        groups_.push_back({execute});
        groups_.push_back({cfg(2,3,true)}); // unread output must survive rejection
        groups_.push_back({store(0,{50,50,50,50})});
        groups_.push_back({cfg(2,3)});
        groups_.push_back({badExecute}); // old weights/input cannot satisfy new coverage
        groups_.push_back({input(Operation::Program,0,{1,2,3}), cfg(1,1,true)});
        groups_.push_back({input(Operation::Program,0,{1,2,3})}); // duplicate coverage
        auto bad = input(Operation::Program,3,{7}); bad.error = true; groups_.push_back({bad});
        bad = input(Operation::Program,2,{7,7,7,7}); bad.error = true; groups_.push_back({bad});
        groups_.push_back({badExecute});
        groups_.push_back({input(Operation::Program,5,{4,5,6})}); // physical row stride = 5
        bad = input(Operation::Load,0,{1,2,3,4}); bad.error = true; groups_.push_back({bad});
        groups_.push_back({Step{Operation::Store,0,3,{}, {},true}});
        groups_.push_back({input(Operation::Load,0,{1,2})});
        groups_.push_back({badExecute});
        groups_.push_back({input(Operation::Load,2,{3})});
        if (p.find<bool>("pipeline", true)) {
            // Load for invocation 2 overlaps invocation 1, whose captured
            // input and active shape must remain unchanged.
            groups_.push_back({execute, input(Operation::Load,0,{2,1,0})});
            groups_.push_back({execute, cfg(1,1,true)});
            groups_.push_back({cfg(1,1,true)}); // two completed unread results
            // A third compute waits for BOTH active rows, not physical rows.
            // Re-reading row 0 must not release its output slot early.
            auto read0 = store(0,{14}); read0.deferred = true;
            auto read1 = store(1,{32}); read1.deferred = true;
            groups_.push_back({execute, read0, read0, read1});
            groups_.push_back({store(0,{4,13})});
            groups_.push_back({store(0,{4,13})});
        } else {
            groups_.push_back({execute});
            groups_.push_back({store(0,{14})});
            groups_.push_back({store(0,{14})});
            groups_.push_back({cfg(1,1,true)});
            groups_.push_back({store(1,{32})});
            groups_.push_back({input(Operation::Load,0,{2,1,0})});
            groups_.push_back({execute});
            groups_.push_back({store(0,{4,13})});
        }
        groups_.push_back({cfg(1,1)}); // long -> short invalidates old tails
        groups_.push_back({badExecute});
        groups_.push_back({input(Operation::Program,0,{-2})});
        groups_.push_back({Step{Operation::Load,1,0}}); // zero VL cannot initialize input
        groups_.push_back({badExecute});
        groups_.push_back({input(Operation::Load,0,{3})});
        groups_.push_back({execute});
        groups_.push_back({store(0,{-6})});
        groups_.push_back({cfg(2,3)}); // short -> long cannot resurrect old values
        groups_.push_back({badExecute});
    }
    void finish() override {
        check(done_ && live_.empty(), "unfinished protocol");
        std::cout << "ACTIVE_REGION_RESULT {\"passed\":true,\"completed\":" << completed_
                  << ",\"expected_errors\":" << errors_ << "}\n";
    }
private:
    struct Step {
        Operation operation; std::uint64_t offset = 0; unsigned count = 0;
        std::vector<std::uint8_t> payload, expected;
        bool error = false; unsigned array = 0; bool deferred = false;
    };
    struct Pending { Step step; bool accepted = false, started = false; std::uint64_t start = 0; };
    SST::Link* commands_;
    SST::TimeConverter clock_;
    std::vector<std::vector<Step>> groups_;
    std::map<std::uint64_t, Pending> live_;
    std::size_t next_ = 0;
    std::uint64_t token_ = 0;
    unsigned completed_ = 0, errors_ = 0;
    bool done_ = false;
    void check(bool ok, const char* message) {
        if (!ok) fatal(CALL_INFO, -1, "active-region protocol group %zu: %s\n", next_, message);
    }
    static std::vector<std::uint8_t> encode(const std::vector<float>& values) {
        std::vector<std::uint8_t> bytes;
        for (float value : values) {
            std::uint32_t bits; std::memcpy(&bits, &value, 4);
            for (unsigned b = 0; b < 4; ++b) bytes.push_back(std::uint8_t(bits >> (8*b)));
        }
        return bytes;
    }
    bool tick(SST::Cycle_t cycle) {
        check(cycle < 100000, "progress timeout");
        if (!live_.empty()) return false;
        if (next_ == groups_.size()) {
            done_ = true; primaryComponentOKToEndSim(); return true;
        }
        for (const auto& step : groups_[next_++]) {
            auto* request = new ArrayCommand;
            request->token = ++token_; request->operation = step.operation;
            request->elementOffset = step.offset; request->elementCount = step.count;
            request->array = step.array; request->deferred = step.deferred; request->payload = step.payload;
            live_.emplace(token_, Pending{step}); commands_->send(request);
        }
        return false;
    }
    void response(SST::Event* event) {
        std::unique_ptr<SST::Event> owned(event);
        auto* reply = dynamic_cast<ArrayCommand*>(event);
        check(reply != nullptr, "invalid response");
        auto found = live_.find(reply->token); check(found != live_.end(), "unknown token");
        auto& pending = found->second; const auto& step = pending.step;
        check(reply->operation == step.operation && reply->array == step.array &&
              reply->elementOffset == step.offset && reply->elementCount == step.count,
              "changed response metadata");
        if (reply->status == CommandStatus::Accepted || reply->status == CommandStatus::Guaranteed) {
            check(!pending.accepted && reply->payload.empty(), "duplicate acceptance");
            pending.accepted = true;
        } else if (reply->status == CommandStatus::Started) {
            check(pending.accepted && !pending.started && step.operation == Operation::Execute,
                  "invalid Started response");
            pending.started = true; pending.start = reply->cycle;
        } else if (step.error) {
            check(reply->status == CommandStatus::Error && reply->payload.empty(), "expected rejected command");
            ++errors_; live_.erase(found);
        } else {
            check(reply->status == CommandStatus::Complete && pending.accepted, "unexpected terminal status");
            check(reply->payload == step.expected, "incorrect result or non-store payload");
            if (pending.started) check(reply->cycle - pending.start == 100, "compute latency changed");
            ++completed_; live_.erase(found);
        }
    }
};
}
