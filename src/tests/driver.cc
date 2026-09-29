#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/link.h>
#include "../components/analog-arrays/commands.h"
#include "../components/fixed.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace TileComponents {
// Independent fixtures: SPM probes and constant register payloads. Array data
// never passes through this driver's memory interface.
class Driver final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(Driver, "tilecomponents", "Driver",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Independent component regression fixtures", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"rows", "Array rows", "7"}, {"cols", "Array columns", "13"},
        {"arrays", "Array count", "2"}, {"chunk_elements", "Register chunk elements", "9"},
        {"spm_request_bytes", "SPM ordering-line size", "32"},
        {"spm_bank_width", "SPM bank stripe bytes", "4"}, {"spm_banks", "SPM banks", "8"},
        {"scenario", "arrays, protocol, duplex, memory, same-bank, memory-vector", "arrays"})
    SST_ELI_DOCUMENT_PORTS({"commands", "Register-payload array commands", {"TileComponents.ArrayCommand"}})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({"memory", "Independent SPM probes", "SST::Interfaces::StandardMem"})
    Driver(SST::ComponentId_t id, SST::Params& p) : Component(id),
        rows_(p.find<unsigned>("rows", 7)), cols_(p.find<unsigned>("cols", 13)),
        arrays_(p.find<unsigned>("arrays", 2)), chunk_(p.find<unsigned>("chunk_elements", 9)),
        requestBytes_(p.find<unsigned>("spm_request_bytes", 32)),
        bankWidth_(p.find<unsigned>("spm_bank_width", 4)), banks_(p.find<unsigned>("spm_banks", 8)),
        scenario_(p.find<std::string>("scenario", "arrays")) {
        clock_ = registerClock(Clock, new SST::Clock::Handler<Driver, &Driver::tick>(this));
        memory_ = loadUserSubComponent<Memory>("memory", SST::ComponentInfo::SHARE_NONE, clock_,
            new Memory::Handler<Driver, &Driver::memoryResponse>(this));
        commands_ = configureLink("commands", Clock,
            new SST::Event::Handler<Driver, &Driver::commandResponse>(this));
        if (!memory_ || !commands_ || !rows_ || !cols_ || !arrays_ || !chunk_)
            fatal(CALL_INFO, -1, "invalid component test fixture\n");
        if (!memoryScenario()) makeSteps();
        if (scenario_ == "memory-vector" && (requestBytes_ != 32 || bankWidth_ != 4 || banks_ != 8))
            fatal(CALL_INFO, -1, "memory-vector requires 32-byte requests and eight 4-byte banks\n");
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void init(unsigned phase) override { memory_->init(phase); }
    void setup() override { memory_->setup(); }
    void finish() override {
        if (!done_ || !memoryPending_.empty() || !commandPending_.empty() || duplicatePending_)
            fatal(CALL_INFO, -1, "component test ended before verification\n");
        std::cout << "TEST_RESULT {\"passed\":true,\"scenario\":\"" << scenario_
            << "\",\"verified_bytes\":" << verified_ << ",\"memory_start\":" << memoryStart_
            << ",\"memory_end\":" << memoryEnd_ << ",\"measured_cycles\":" << measuredCycles_
            << ",\"busy\":" << busy_ << ",\"errors\":" << errors_
            << ",\"programs\":" << programs_ << ",\"executions\":" << executions_
            << ",\"end_cycle\":" << getCurrentSimTime(clock_) << "}\n";
    }
private:
    using Memory = SST::Interfaces::StandardMem;
    struct Action {
        unsigned array; Operation operation; std::uint64_t offset; unsigned count;
        std::vector<std::uint8_t> payload, expected;
        CommandStatus terminal = CommandStatus::Complete;
        bool mustAccept = true;
        CommandStatus requestStatus = CommandStatus::Request;
    };
    struct CommandPending { Action action; bool accepted = false; };
    struct MemoryPending { bool write; std::vector<std::uint8_t> expected; };
    Memory* memory_; SST::Link* commands_; SST::TimeConverter clock_;
    unsigned rows_, cols_, arrays_, chunk_, requestBytes_, bankWidth_, banks_;
    std::string scenario_;
    std::vector<std::vector<Action>> steps_;
    std::map<std::uint64_t, CommandPending> commandPending_;
    std::map<Memory::Request::id_t, MemoryPending> memoryPending_;
    std::uint64_t nextToken_ = 1, duplicateToken_ = 0, measuredStart_ = 0, measuredCycles_ = 0;
    std::uint64_t verified_ = 0, memoryStart_ = 0, memoryEnd_ = 0;
    unsigned step_ = 0, memoryPhase_ = 0, busy_ = 0, errors_ = 0, programs_ = 0, executions_ = 0;
    bool done_ = false, duplicatePending_ = false;
    bool memoryScenario() const {
        return scenario_ == "memory" || scenario_ == "same-bank" || scenario_ == "memory-vector";
    }
    static std::vector<std::uint8_t> encode(const std::vector<float>& values) {
        std::vector<std::uint8_t> bytes(values.size() * 4);
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::uint32_t bits; std::memcpy(&bits, &values[i], 4);
            for (unsigned b = 0; b < 4; ++b) bytes[4*i+b] = std::uint8_t(bits >> (8*b));
        }
        return bytes;
    }
    float weight(unsigned a, unsigned index, bool changed) const {
        return changed && index == 0 ? 2.5f : (int((index * 3 + a) % 9) - 4) * 0.125f;
    }
    float input(unsigned index, bool changed) const {
        return changed && index == 0 ? 3.5f : float(int(index % 7) - 3);
    }
    Action transfer(unsigned a, Operation op, unsigned offset, unsigned count, bool changed = false) const {
        Action result{a, op, offset, count, {}, {}};
        std::vector<float> data(count);
        for (unsigned i = 0; i < count; ++i) {
            if (op == Operation::Program) data[i] = weight(a, offset+i, changed);
            else if (op == Operation::Load) data[i] = input(offset+i, changed);
            else {
                double sum = 0;
                for (unsigned c = 0; c < cols_; ++c)
                    sum += double(weight(a, (offset+i)*cols_+c, changed)) * input(c, changed);
                data[i] = float(sum);
            }
        }
        if (op == Operation::Store) result.expected = encode(data);
        else result.payload = encode(data);
        return result;
    }
    void all(Operation op, unsigned offset, unsigned count, bool changed = false) {
        std::vector<Action> batch;
        for (unsigned a = 0; a < arrays_; ++a) batch.push_back(transfer(a, op, offset, count, changed));
        steps_.push_back(std::move(batch));
    }
    void chunks(Operation op, unsigned total, bool changed = false) {
        for (unsigned offset = 0; offset < total; offset += std::min(chunk_, total-offset))
            all(op, offset, std::min(chunk_, total-offset), changed);
    }
    void execute(bool expectError = false) {
        all(Operation::Execute, 0, 0);
        if (expectError) for (auto& a : steps_.back()) a.terminal = CommandStatus::Error;
    }
    void makeSteps() {
        if (scenario_ == "protocol") {
            // Four live commands fill the queue; the fifth must be rejected.
            std::vector<Action> batch;
            batch.push_back(transfer(0, Operation::Program, 0, rows_*cols_));
            batch.push_back(transfer(0, Operation::Load, 0, cols_));
            batch.push_back(transfer(0, Operation::Execute, 0, 0));
            batch.push_back(transfer(0, Operation::Store, 0, rows_));
            auto busy = transfer(0, Operation::Load, 0, cols_);
            busy.terminal = CommandStatus::Busy; busy.mustAccept = false; batch.push_back(busy);
            auto invalid = [&](Action value) {
                value.terminal = CommandStatus::Error; value.mustAccept = false; batch.push_back(std::move(value));
            };
            invalid(transfer(arrays_, Operation::Load, 0, 0));
            invalid(transfer(0, Operation::Load, cols_+1, 0));
            auto range = transfer(0, Operation::Load, 0, 1); range.offset = UINT64_MAX; invalid(range);
            auto payload = transfer(0, Operation::Program, 0, 1); payload.payload.pop_back(); invalid(payload);
            auto store = transfer(0, Operation::Store, 0, 1); store.payload = {0}; invalid(store);
            auto exec = transfer(0, Operation::Execute, 0, 0); exec.offset = 1; invalid(exec);
            auto opcode = exec; opcode.offset = 0; opcode.operation = static_cast<Operation>(255); invalid(opcode);
            auto status = transfer(0, Operation::Load, 0, 0); status.requestStatus = CommandStatus::Complete; invalid(status);
            steps_.push_back(std::move(batch)); return;
        }
        if (scenario_ == "duplex") {
            if (arrays_ < 2) fatal(CALL_INFO, -1, "duplex fixture requires two arrays\n");
            all(Operation::Program, 0, rows_*cols_); all(Operation::Load, 0, cols_); execute();
            steps_.push_back({transfer(0, Operation::Store, 0, rows_), transfer(1, Operation::Load, 0, cols_)});
            return;
        }
        if (scenario_ != "arrays") fatal(CALL_INFO, -1, "unknown component test scenario\n");
        execute(true);
        all(Operation::Store, 0, 1);
        for (auto& a : steps_.back()) { a.terminal = CommandStatus::Error; a.expected.clear(); }
        all(Operation::Program, rows_*cols_, 0); all(Operation::Load, cols_, 0); all(Operation::Store, rows_, 0);
        all(Operation::Program, 0, 1); all(Operation::Load, 0, 1); execute(rows_*cols_ > 1 || cols_ > 1);
        chunks(Operation::Program, rows_*cols_); chunks(Operation::Load, cols_); execute(); chunks(Operation::Store, rows_);
        all(Operation::Program, 0, 1, true); all(Operation::Load, 0, 1, true); execute(); chunks(Operation::Store, rows_, true);
        // Empty transfers preserve the computed result and initialization state.
        all(Operation::Program, rows_*cols_, 0); all(Operation::Load, cols_, 0); all(Operation::Store, rows_, 0);
        chunks(Operation::Store, rows_, true);
        all(Operation::Program, 0, 1); all(Operation::Load, 0, 1); execute(); chunks(Operation::Store, rows_);
    }
    void launchStep() {
        const auto firstToken = nextToken_;
        if (step_ + 1 == steps_.size()) measuredStart_ = getCurrentSimTime(clock_);
        for (const auto& action : steps_[step_]) {
            auto* request = new ArrayCommand;
            request->array = action.array; request->operation = action.operation;
            request->elementOffset = action.offset; request->elementCount = action.count;
            request->payload = action.payload; request->status = action.requestStatus; request->token = nextToken_++;
            commandPending_.emplace(request->token, CommandPending{action}); commands_->send(request);
        }
        if (scenario_ == "protocol") {
            auto* duplicate = new ArrayCommand;
            duplicate->token = firstToken; duplicateToken_ = firstToken; duplicatePending_ = true;
            commands_->send(duplicate);
        }
        ++step_;
    }
    void memoryProbe(bool write) {
        // Two simultaneous full vector-sized requests each touch all eight
        // default banks. They must compete for those banks' single ports.
        const bool vector = scenario_ == "memory-vector";
        const unsigned probes = vector ? 2 : 128;
        const unsigned probeBytes = vector ? 32 : bankWidth_;
        for (unsigned i = 0; i < probes; ++i) {
            const auto stride = std::uint64_t(probeBytes) * (scenario_ == "same-bank" ? banks_ : 1);
            const auto address = 4096 + i * stride;
            for (unsigned offset = 0; offset < probeBytes;) {
                const unsigned n = std::min<unsigned>(probeBytes-offset, requestBytes_-(address+offset)%requestBytes_);
                std::vector<std::uint8_t> bytes(n);
                for (unsigned j = 0; j < n; ++j) bytes[j] = std::uint8_t(i*13+offset+j);
                Memory::Request* request = write ? static_cast<Memory::Request*>(new Memory::Write(address+offset, n, bytes)) :
                                                  static_cast<Memory::Request*>(new Memory::Read(address+offset, n));
                request->setNoncacheable(); memoryPending_.emplace(request->getID(), MemoryPending{write, std::move(bytes)});
                memory_->send(request); offset += n;
            }
        }
    }
    bool tick(SST::Cycle_t) {
        const auto now = getCurrentSimTime(clock_);
        if (now > 10000000) fatal(CALL_INFO, -1, "component test progress timeout\n");
        if (memoryScenario()) {
            if (!memoryPhase_) { memoryProbe(true); memoryPhase_ = 1; }
            else if (memoryPending_.empty() && memoryPhase_ == 1) {
                memoryStart_ = now; memoryProbe(false); memoryPhase_ = 2;
            } else if (memoryPending_.empty()) { memoryEnd_ = now; done_ = true; }
        } else if (commandPending_.empty() && !duplicatePending_) {
            if (step_ < steps_.size()) launchStep();
            else { measuredCycles_ = now-measuredStart_; done_ = true; }
        }
        if (done_) { primaryComponentOKToEndSim(); return true; }
        return false;
    }
    void memoryResponse(Memory::Request* response) {
        const auto found = memoryPending_.find(response->getID());
        if (found == memoryPending_.end()) fatal(CALL_INFO, -1, "unexpected SPM response\n");
        if (found->second.write) {
            if (!dynamic_cast<Memory::WriteResp*>(response)) fatal(CALL_INFO, -1, "missing SPM write response\n");
        } else {
            const auto* read = dynamic_cast<Memory::ReadResp*>(response);
            if (!read || read->data != found->second.expected) fatal(CALL_INFO, -1, "SPM data mismatch\n");
            verified_ += read->data.size();
        }
        memoryPending_.erase(found); delete response;
    }
    void commandResponse(SST::Event* event) {
        const auto* response = dynamic_cast<ArrayCommand*>(event);
        if (!response) fatal(CALL_INFO, -1, "invalid array response event\n");
        if (duplicatePending_ && response->token == duplicateToken_ && response->status == CommandStatus::Error) {
            duplicatePending_ = false; ++errors_; delete response; return;
        }
        auto found = commandPending_.find(response->token);
        if (found == commandPending_.end()) fatal(CALL_INFO, -1, "unexpected array response token\n");
        auto& pending = found->second; const auto& action = pending.action;
        if (response->array != action.array || response->operation != action.operation ||
            response->elementOffset != action.offset || response->elementCount != action.count)
            fatal(CALL_INFO, -1, "array response changed command identity\n");
        if (response->status == CommandStatus::Accepted) {
            if (!action.mustAccept || pending.accepted || !response->payload.empty())
                fatal(CALL_INFO, -1, "unexpected array acceptance\n");
            pending.accepted = true;
        } else {
            if (response->status != action.terminal || pending.accepted != action.mustAccept ||
                response->payload != (action.terminal == CommandStatus::Complete ? action.expected : std::vector<std::uint8_t>{}))
                fatal(CALL_INFO, -1, "array completion/status/numerical result mismatch\n");
            if (response->status == CommandStatus::Busy) ++busy_;
            else if (response->status == CommandStatus::Error) ++errors_;
            else {
                verified_ += response->payload.size();
                programs_ += action.operation == Operation::Program && action.count != 0;
                executions_ += action.operation == Operation::Execute;
            }
            commandPending_.erase(found);
        }
        delete response;
    }
};
}
