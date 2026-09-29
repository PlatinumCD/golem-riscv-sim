#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "../../components/analog-arrays/commands.h"
#include <cstring>
#include <iostream>
#include <map>

namespace TileComponents {
class ProgrammingDelayProtocol final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(ProgrammingDelayProtocol, "tilecomponents", "ProgrammingDelayProtocol",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Initial programming coverage and timing regression", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"initial", "Initial-full-array scope", "true"},
        {"pipeline", "Execution pipeline", "false"}, {"arrays", "Array count", "1"},
        {"delay", "Programming delay", "0"})
    SST_ELI_DOCUMENT_PORTS({"commands", "Array commands", {"TileComponents.ArrayCommand"}})
    ProgrammingDelayProtocol(SST::ComponentId_t id, SST::Params& p) : Component(id),
        initial_(p.find<bool>("initial", true)), pipeline_(p.find<bool>("pipeline", false)),
        arrays_(p.find<unsigned>("arrays", 1)), delay_(p.find<unsigned>("delay", 0)) {
        clock_ = registerClock("1GHz", new SST::Clock::Handler<ProgrammingDelayProtocol, &ProgrammingDelayProtocol::tick>(this));
        commands_ = configureLink("commands", "1ns",
            new SST::Event::Handler<ProgrammingDelayProtocol, &ProgrammingDelayProtocol::response>(this));
        check(commands_ && arrays_, "missing link or arrays");
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void finish() override {
        check(done_ && live_.empty() && probes_.empty(), "unfinished commands");
        check(verified_ == 4 * arrays_, "missing numerical checks");
        std::cout << "PROGRAMMING_PROTOCOL_RESULT {\"passed\":true,\"arrays\":" << arrays_
            << ",\"completed\":" << completed_ << ",\"errors\":" << errors_
            << ",\"verified_values\":" << verified_ << ",\"during_delay_rejections\":" << duringRejected_ << "}\n";
    }
private:
    struct Pending {
        unsigned array, tag, offset, count;
        Operation operation;
        CommandStatus expected;
        bool mustAccept, accepted = false, started = false;
        std::vector<std::uint8_t> result;
    };
    SST::Link* commands_;
    SST::TimeConverter clock_;
    bool initial_, pipeline_, launched_ = false, done_ = false;
    unsigned arrays_, delay_, step_ = 0, completed_ = 0, errors_ = 0, verified_ = 0, duringRejected_ = 0;
    std::map<std::uint64_t, Pending> live_;
    std::map<unsigned, std::uint64_t> probes_;

    void check(bool value, const char* message) {
        if (!value) fatal(CALL_INFO, -1, "programming-delay protocol: %s\n", message);
    }
    static std::vector<std::uint8_t> encode(std::initializer_list<float> values) {
        std::vector<std::uint8_t> bytes;
        for (float value : values) {
            std::uint32_t bits; std::memcpy(&bits, &value, 4);
            for (unsigned b = 0; b < 4; ++b) bytes.push_back(std::uint8_t(bits >> (8*b)));
        }
        return bytes;
    }
    void send(unsigned array, unsigned tag, Operation op, unsigned offset = 0, unsigned count = 0,
              std::vector<std::uint8_t> payload = {}, CommandStatus expected = CommandStatus::Complete,
              bool accept = true, std::vector<std::uint8_t> result = {}) {
        auto* request = new ArrayCommand;
        request->array = array; request->token = 100 * array + tag; request->operation = op;
        request->elementOffset = offset; request->elementCount = count; request->payload = std::move(payload);
        check(live_.emplace(request->token, Pending{array, tag, offset, count, op, expected, accept,
                                                  false, false, std::move(result)}).second, "duplicate token");
        commands_->send(request);
    }
    void launch() {
        if (step_ == 10) {
            check(live_.empty() && probes_.empty(), "exit left a probe");
            done_ = true; primaryComponentOKToEndSim(); return;
        }
        for (unsigned a = 0; a < arrays_; ++a) {
            switch (step_) {
            case 0: send(a, 1, Operation::Load, 0, 3, encode({1, 1, 1})); break;
            case 1: send(a, 2, Operation::Program, 4, 2, encode({5, 6})); break;
            case 2: send(a, 3, Operation::Program, 4, 2, encode({50, 60})); break;
            case 3: send(a, 4, Operation::Execute, 0, 0, {}, CommandStatus::Error); break;
            case 4: send(a, 5, Operation::Program, 6, 0); break;
            case 5: send(a, 6, Operation::Program, 0, 2, encode({1, 2})); break;
            case 6:
                // Overlap one old element, then fill the two remaining holes.
                // All four requests arrive before the first transfer completes.
                send(a, 7, Operation::Program, 1, 3, encode({20, 3, 4}));
                send(a, 8, Operation::Program, 0, 1, encode({999}),
                     initial_ ? CommandStatus::Error : CommandStatus::Complete);
                send(a, 9, Operation::Program, 6, 0);
                send(a, 10, Operation::Execute);
                break;
            case 7:
                send(a, 11, Operation::Store, 0, 2, {}, CommandStatus::Complete, true,
                     initial_ ? encode({24, 114}) : encode({1022, 114})); break;
            case 8:
                send(a, 13, Operation::Program, 6, 0);
                send(a, 14, Operation::Program, 0, 1, encode({7}),
                     initial_ ? CommandStatus::Error : CommandStatus::Complete, !initial_);
                send(a, 15, Operation::Execute);
                break;
            case 9:
                send(a, 16, Operation::Store, 0, 2, {}, CommandStatus::Complete, true,
                     initial_ ? encode({24, 114}) : encode({30, 114})); break;
            }
        }
    }
    bool tick(SST::Cycle_t cycle) {
        check(cycle < 100000, "timeout");
        if (!launched_) { launched_ = true; launch(); }
        const auto now = getCurrentSimTime(clock_);
        for (auto it = probes_.begin(); it != probes_.end();) {
            if (now < it->second) { ++it; continue; }
            send(it->first, 21, Operation::Program, 0, 1, encode({888}), CommandStatus::Error, false);
            it = probes_.erase(it);
        }
        return done_;
    }
    void response(SST::Event* event) {
        auto* reply = dynamic_cast<ArrayCommand*>(event);
        check(reply != nullptr, "invalid reply");
        auto found = live_.find(reply->token);
        check(found != live_.end(), "unknown token");
        auto& pending = found->second;
        check(reply->array == pending.array && reply->operation == pending.operation &&
              reply->elementOffset == pending.offset && reply->elementCount == pending.count, "changed metadata");
        if (reply->status == CommandStatus::Accepted) {
            check(pending.mustAccept && !pending.accepted && reply->payload.empty(), "invalid acceptance");
            pending.accepted = true;
            if (initial_ && pending.tag == 7 && delay_ >= 16)
                probes_[pending.array] = getCurrentSimTime(clock_) + 4;
        } else if (reply->status == CommandStatus::Started) {
            check(pipeline_ && pending.accepted && !pending.started && pending.operation == Operation::Execute &&
                  pending.expected == CommandStatus::Complete && reply->payload.empty(), "invalid compute start");
            pending.started = true;
        } else {
            check(reply->status == pending.expected && pending.accepted == pending.mustAccept, "wrong terminal status/order");
            check(reply->payload == pending.result, "wrong numerical output or unexpected payload");
            if (pending.expected == CommandStatus::Complete) {
                ++completed_;
                if (pending.operation == Operation::Store) verified_ += pending.count;
                if (pending.operation == Operation::Execute) check(pending.started == pipeline_, "missing compute start");
            } else {
                ++errors_;
                if (pending.tag == 21) ++duringRejected_;
            }
            live_.erase(found);
            if (live_.empty()) { ++step_; launch(); }
        }
        delete reply;
    }
};
}
