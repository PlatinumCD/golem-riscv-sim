#include <sst/core/sst_config.h>
#include "riscvQemu.h"
#include "../observations.h"

namespace TileComponents {
void RiscvQemu::prepareInstructionIssue() {
    if (issuePending_) throw std::logic_error("overlapping instruction admissions");
    issueInstruction_ = Riscv::decodeInstructionTiming(event_->instructionBits,
        event_->memorySize, event_->instructionVtype);
    const auto now = getCurrentSimTime(clock_);
    const auto decision = issue_.earliest(issueInstruction_, now);
    issuePending_ = true;
    const auto wait = decision.cycle - now;
    issueWaitCycles_ += wait;
    if (wait) {
        if (RecordComponentObservations && issueWaitTrace_)
            issueWaitTrace_ << now << ',' << decision.cycle << ',' << decision.reason << ',' << event_->memoryAddress << '\n';
        issueWake_->send(wait, new SST::Event);
    } else issueInstruction(nullptr);
}

void RiscvQemu::issueInstruction(SST::Event* wake) {
    delete wake;
    try {
        if (!issuePending_ || event_->stopReason != MITTENS_SYNC_STOP_INSTRUCTION_FETCH)
            throw std::logic_error("instruction issue wake without a fetched instruction");
        const auto now = getCurrentSimTime(clock_);
        const auto ready = issue_.issue(issueInstruction_, now);
        if (RecordComponentObservations && issueTrace_)
            issueTrace_ << now << ',' << event_->memoryAddress << ',' << event_->instructionBits << ','
                << Riscv::issueUnitName(issueInstruction_.unit) << ',' << ready << '\n';
        // A redirect/control instruction ends the sequential fetch block.
        if (issueInstruction_.endsFetchBlock) fetchRemaining_ = 0;
        issuePending_ = false;
        resume();
    } catch (const std::exception& error) { fail(error.what()); }
}
}
