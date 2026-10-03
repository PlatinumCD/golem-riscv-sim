#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace TileComponents::Riscv {

// x0..x31, f0..f31, v0..v31, and the implicit VL/VTYPE dependency.
using RegisterSet = std::bitset<97>;
enum class IssueUnit : unsigned { Integer, Memory, Vector, Floating, Multiply, Divide, Control, Serial, Count };
inline const char* issueUnitName(IssueUnit unit) {
    constexpr const char* names[] = {"integer", "memory", "vector", "floating", "multiply", "divide", "control", "serial"};
    return names[unsigned(unit)];
}
struct InstructionTiming {
    RegisterSet reads, writes;
    IssueUnit unit = IssueUnit::Serial;
    bool serialize = true, endsFetchBlock = true;
};
InstructionTiming decodeInstructionTiming(std::uint32_t bits, unsigned bytes, std::uint64_t vtype);

struct IssueUnitTiming { unsigned count = 1; std::uint64_t latency = 1, interval = 1; };
struct IssueConfiguration {
    unsigned width = 1;
    std::array<IssueUnitTiming, unsigned(IssueUnit::Count)> units{};
    IssueConfiguration() {
        units[unsigned(IssueUnit::Integer)].count = 2;
        units[unsigned(IssueUnit::Floating)].latency = 3;
        units[unsigned(IssueUnit::Multiply)].latency = 3;
        units[unsigned(IssueUnit::Divide)] = {1, 16, 16};
    }
};

// In-order issue, operand capture at issue, RAW/WAW scoreboarding, no speculation
// or renaming. Dynamic memory/array completion remains owned by the existing
// QEMU/SST queues; their barriers run before this scheduler admits an instruction.
class InstructionIssue final {
public:
    struct Decision { std::uint64_t cycle; unsigned lane; const char* reason; };
    explicit InstructionIssue(IssueConfiguration config = {}) : config_(config) {
        if (!config.width || config.width > 4) throw std::invalid_argument("issue_width must be 1..4");
        for (const auto& unit : config.units)
            if (!unit.count || unit.count > 4 || !unit.latency || unit.latency > 1024 ||
                !unit.interval || unit.interval > 1024)
                throw std::invalid_argument("invalid instruction unit count, latency or initiation interval");
    }
    const IssueConfiguration& configuration() const { return config_; }
    static std::uint64_t add(std::uint64_t a, std::uint64_t b) {
        if (b > std::numeric_limits<std::uint64_t>::max() - a) throw std::overflow_error("instruction timing overflow");
        return a + b;
    }
    Decision earliest(const InstructionTiming& instruction, std::uint64_t now) const {
        Decision d{std::max(now, blockedUntil_), 0, "serialization"};
        if (issueCount_ && lastCycle_ == d.cycle && (used_ == config_.width || instruction.serialize)) {
            d.cycle = add(d.cycle, 1); d.reason = "width";
        }
        auto operands = instruction.reads | instruction.writes;
        if (instruction.serialize) operands.set();
        for (unsigned r = 1; r < ready_.size(); ++r)
            if (operands[r] && ready_[r] > d.cycle) { d.cycle = ready_[r]; d.reason = "register"; }
        const unsigned u = unsigned(instruction.unit);
        for (unsigned lane = 1; lane < config_.units[u].count; ++lane)
            if (available_[u][lane] < available_[u][d.lane]) d.lane = lane;
        if (available_[u][d.lane] > d.cycle) { d.cycle = available_[u][d.lane]; d.reason = "unit"; }
        return d;
    }
    std::uint64_t issue(const InstructionTiming& instruction, std::uint64_t cycle) {
        const auto d = earliest(instruction, cycle);
        if (d.cycle != cycle) throw std::logic_error("instruction issued before dependencies/resources were ready");
        if (!issueCount_ || cycle != lastCycle_) { ++issueCycles_; used_ = 0; }
        if (issueCount_ && cycle < lastCycle_) throw std::logic_error("instruction issue time went backwards");
        lastCycle_ = cycle; ++used_; ++issueCount_;
        peakWidth_ = std::max(peakWidth_, used_);
        const unsigned u = unsigned(instruction.unit);
        const auto result = add(cycle, config_.units[u].latency);
        available_[u][d.lane] = add(cycle, config_.units[u].interval);
        for (unsigned r = 1; r < ready_.size(); ++r) if (instruction.writes[r]) ready_[r] = result;
        if (instruction.serialize) blockedUntil_ = result;
        ++unitIssues_[u];
        return result;
    }
    std::uint64_t issueCycles() const { return issueCycles_; }
    std::uint64_t instructions() const { return issueCount_; }
    unsigned peakWidth() const { return peakWidth_; }
private:
    IssueConfiguration config_;
    std::array<std::uint64_t, 97> ready_{};
    std::array<std::array<std::uint64_t, 4>, unsigned(IssueUnit::Count)> available_{};
    std::array<std::uint64_t, unsigned(IssueUnit::Count)> unitIssues_{};
    std::uint64_t blockedUntil_ = 0, lastCycle_ = 0, issueCount_ = 0, issueCycles_ = 0;
    unsigned used_ = 0, peakWidth_ = 0;
};
}
