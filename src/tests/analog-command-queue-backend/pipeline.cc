#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "commands.h"
#include <cstring>
#include <iostream>
#include <map>

namespace TileComponents {
class PipelineProtocol final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(PipelineProtocol, "tilecomponents", "PipelineProtocol",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Array pipeline backpressure regression", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"execute_cycles", "Expected execution latency", "4000"},
        {"deferred", "Enable deferred transfers", "false"})
    SST_ELI_DOCUMENT_PORTS({"commands", "Array commands", {"TileComponents.ArrayCommand"}})
    PipelineProtocol(SST::ComponentId_t id, SST::Params& params) : Component(id),
        executeCycles_(params.find<unsigned>("execute_cycles", 4000)), deferred_(params.find<bool>("deferred", false)) {
        registerClock("1GHz", new SST::Clock::Handler<PipelineProtocol, &PipelineProtocol::tick>(this));
        commands_ = configureLink("commands", "1ns",
            new SST::Event::Handler<PipelineProtocol, &PipelineProtocol::response>(this));
        if (!commands_) fatal(CALL_INFO, -1, "missing protocol command link\n");
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void finish() override {
        check(done_ && live_.empty() && completed_ == 12 && computes_ == 3,
              "protocol did not finish every accepted command");
        std::cout << "PIPELINE_PROTOCOL_RESULT {\"passed\":true,\"commands\":" << completed_
                  << ",\"computes\":" << computes_
                  << ",\"third_execute_backpressured\":true,\"duplicate_coverage_preserved\":true}\n";
    }
private:
    struct Pending {
        Operation operation; unsigned offset, count;
        bool accepted = false, started = false;
        std::uint64_t start = 0;
    };
    SST::Link* commands_;
    std::map<std::uint64_t, Pending> live_;
    unsigned executeCycles_, completed_ = 0, computes_ = 0;
    bool deferred_ = false, batchSent_ = false, input3Ready_ = false;
    bool launched_ = false, done_ = false, thirdStarted_ = false, thirdAccepted_ = false;

    void check(bool value, const char* message) {
        if (!value) fatal(CALL_INFO, -1, "array pipeline protocol: %s\n", message);
    }
    static std::vector<std::uint8_t> encode(std::initializer_list<float> values) {
        std::vector<std::uint8_t> bytes;
        for (float value : values) {
            std::uint32_t bits; std::memcpy(&bits, &value, 4);
            for (unsigned b = 0; b < 4; ++b) bytes.push_back(std::uint8_t(bits >> (8*b)));
        }
        return bytes;
    }
    void send(std::uint64_t token, Operation op, unsigned offset = 0, unsigned count = 0,
              std::vector<std::uint8_t> payload = {}) {
        auto* command = new ArrayCommand;
        command->token = token; command->operation = op;
        command->elementOffset = offset; command->elementCount = count;
        command->payload = std::move(payload);
        command->deferred = deferred_ && op != Operation::Execute;
        check(live_.emplace(token, Pending{op, offset, count}).second, "duplicate fixture token");
        commands_->send(command);
    }
    bool tick(SST::Cycle_t cycle) {
        check(cycle < 100000, "progress timeout");
        if (!launched_) {
            launched_ = true;
            send(1, Operation::Program, 0, 4, encode({1, 0, 0, 1}));
        }
        return done_;
    }
    void response(SST::Event* event) {
        auto* reply = dynamic_cast<ArrayCommand*>(event);
        check(reply != nullptr, "invalid response type");
        auto found = live_.find(reply->token);
        check(found != live_.end(), "unknown response token");
        auto& pending = found->second;
        check(reply->operation == pending.operation && reply->array == 0 &&
              reply->elementOffset == pending.offset && reply->elementCount == pending.count,
              "response metadata changed");
        if (reply->status == CommandStatus::Captured) {
            check(deferred_ && pending.accepted && (pending.operation == Operation::Program || pending.operation == Operation::Load), "bad Captured");
            delete reply; return;
        }
        if (reply->status == CommandStatus::Accepted || reply->status == CommandStatus::Guaranteed) {
            check(reply->status == ((deferred_ && pending.operation != Operation::Execute) ? CommandStatus::Guaranteed : CommandStatus::Accepted), "wrong admission guarantee");
            check(!pending.accepted && reply->payload.empty(), "duplicate acceptance or payload");
            pending.accepted = true;
            if (reply->token == 7) thirdAccepted_ = true;
        } else if (reply->status == CommandStatus::Started) {
            check(pending.accepted && !pending.started && pending.operation == Operation::Execute &&
                  reply->payload.empty(), "invalid Started ordering");
            pending.started = true; pending.start = reply->cycle;
            if (reply->token == 3) send(4, Operation::Load, 0, 2, encode({3, 4}));
            else if (reply->token == 5) send(6, Operation::Load, 0, 2, encode({5, 6}));
            else if (reply->token == 7) thirdStarted_ = true;
            else check(false, "unexpected execution token");
        } else {
            check(reply->status == CommandStatus::Complete && pending.accepted, "unexpected terminal status");
            if (pending.operation == Operation::Execute) {
                check(pending.started && reply->cycle - pending.start == executeCycles_,
                      "Complete preceded Started or changed compute latency");
                ++computes_;
            }
            const auto token = reply->token;
            if (pending.operation != Operation::Store) check(reply->payload.empty(), "unexpected completion bytes");
            else if (token == 8 || token == 9) {
                check(reply->payload == encode({1}), "oldest result changed or was retired early");
                check(thirdAccepted_ && !thirdStarted_, "third execution bypassed the full result FIFO");
            } else if (token == 10) check(reply->payload == encode({2}), "partial result was overwritten");
            else if (token == 11) check(reply->payload == encode({3, 4}), "second input snapshot changed");
            else if (token == 12) check(reply->payload == encode({5, 6}), "third input snapshot changed");
            else check(false, "unknown store response");
            live_.erase(found); ++completed_;
            switch (token) {
            case 1: send(2, Operation::Load, 0, 2, encode({1, 2})); break;
            case 2: send(3, Operation::Execute); break;
            case 4: send(5, Operation::Execute); break;
            case 6:
                input3Ready_ = true;
                break;
            case 8: break; // duplicate coverage
            case 9: break; // now retire result 0
            case 10: send(11, Operation::Store, 0, 2); send(12, Operation::Store, 0, 2); break;
            case 11: break;
            case 12:
                check(thirdStarted_ && live_.empty(), "exit left outstanding commands");
                done_ = true; primaryComponentOKToEndSim(); break;
            default: break;
            }
        }
        if (!batchSent_ && input3Ready_ && computes_ == 2) {
            batchSent_ = true; send(7, Operation::Execute);
            send(8, Operation::Store, 0, 1); send(9, Operation::Store, 0, 1); send(10, Operation::Store, 1, 1);
        }
        delete reply;
    }
};
}
