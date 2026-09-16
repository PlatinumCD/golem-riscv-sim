#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../../bridge/include/mittens/MemoryMap.h"

namespace golem::platform {

constexpr uintptr_t ScratchpadBase = MITTENS_SCRATCHPAD_BASE;
constexpr uintptr_t ScratchpadDMARegisterBase = UINT64_C(0x10011000);

enum class ScratchpadDMADirection : uint32_t {
    GlobalRAMToScratchpad = 0,
    ScratchpadToGlobalRAM = 1,
};

enum ScratchpadDMARequestFlags : uint32_t {
    ScratchpadDMAExactReadiness = UINT32_C(1) << 0,
    ScratchpadDMAEpochZeroSource = UINT32_C(1) << 1,
    ScratchpadDMAExactExecutionTeardown = UINT32_C(1) << 2,
};

constexpr uint32_t ScratchpadDMAStatusReady = UINT32_C(1) << 0;
constexpr uint32_t ScratchpadDMAStatusError = UINT32_C(1) << 2;
constexpr uint32_t ScratchpadDMAStatusMacroActive = UINT32_C(1) << 4;
constexpr uint32_t ScratchpadDMACommandSubmit = 1;
constexpr uint32_t ScratchpadDMACommandWait = 2;
constexpr uint32_t ScratchpadDMACommandWaitBatch = 4;
constexpr uint32_t ScratchpadDMACommandMacroBegin = 5;
constexpr uint32_t ScratchpadDMACommandMacroEnd = 6;
constexpr uint32_t ScratchpadDMACommandQuery = 7;
constexpr uint32_t ScratchpadDMACommandAcknowledge = 8;
constexpr uint32_t ScratchpadDMACommandWaitAny = 9;
constexpr uint32_t ScratchpadDMAMaximumJobs = 8;

inline volatile uint32_t& scratchpadDMARegister(uintptr_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(
        ScratchpadDMARegisterBase + offset);
}

inline bool globalDMASubmit(
    uint64_t global_offset,
    uint64_t scratchpad_offset,
    uint32_t byte_count,
    uint32_t token_id,
    uint64_t execution_id,
    uint64_t logical_iteration,
    ScratchpadDMADirection direction,
    uint32_t request_flags = 0
) {
    if ((scratchpadDMARegister(0x28) & ScratchpadDMAStatusReady) == 0) {
        return false;
    }
    scratchpadDMARegister(0x00) = static_cast<uint32_t>(global_offset);
    scratchpadDMARegister(0x04) = static_cast<uint32_t>(global_offset >> 32);
    scratchpadDMARegister(0x08) = static_cast<uint32_t>(scratchpad_offset);
    scratchpadDMARegister(0x0c) = static_cast<uint32_t>(scratchpad_offset >> 32);
    scratchpadDMARegister(0x10) = byte_count;
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x20) = static_cast<uint32_t>(direction);
    scratchpadDMARegister(0x30) = static_cast<uint32_t>(logical_iteration);
    scratchpadDMARegister(0x34) = static_cast<uint32_t>(logical_iteration >> 32);
    scratchpadDMARegister(0x38) = request_flags;
    scratchpadDMARegister(0x24) = ScratchpadDMACommandSubmit;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

inline bool globalDMAWait(uint64_t execution_id, uint32_t token_id) {
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = ScratchpadDMACommandWait;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

enum class GlobalDMACompletion : uint32_t { Pending, Complete, Error };

// Non-destructive, exact-identity query. Complete includes functional copying.
// The caller must retain source/destination ownership until Complete. Querying
// an unknown or already acknowledged identity returns Error, never completion.
inline GlobalDMACompletion globalDMACompletionCommand(
    uint64_t execution_id, uint32_t token_id, uint32_t command) {
    asm volatile("fence iorw, iorw" ::: "memory");
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = command;
    const uint32_t status = scratchpadDMARegister(0x28);
    asm volatile("fence iorw, iorw" ::: "memory");
    if (status & ScratchpadDMAStatusError) return GlobalDMACompletion::Error;
    return status & (UINT32_C(1) << 1) ? GlobalDMACompletion::Complete : GlobalDMACompletion::Pending;
}

inline GlobalDMACompletion globalDMAQuery(uint64_t execution_id, uint32_t token_id) {
    return globalDMACompletionCommand(execution_id, token_id, ScratchpadDMACommandQuery);
}

// Atomically check the DMA and incoming mesh queue, then park if neither is
// ready. Pending means mesh work is available; it never consumes that work or
// retires the DMA. Complete has the same visibility contract as query.
inline GlobalDMACompletion globalDMAWaitForEvent(uint64_t execution_id, uint32_t token_id) {
    return globalDMACompletionCommand(execution_id, token_id, ScratchpadDMACommandWaitAny);
}

// Release one of the eight retained request/completion slots. Pending queries
// never free a slot. This requires a preceding completed query.
inline bool globalDMAAcknowledge(uint64_t execution_id, uint32_t token_id) {
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = ScratchpadDMACommandAcknowledge;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

// Wait for one compiler-certified contiguous token group. Every token still
// names an independent physical DMA request; the device merely resumes the
// guest once after all of their modeled completions have arrived.
inline bool globalDMAWaitBatch(
    uint64_t execution_id,
    uint32_t first_token_id,
    uint32_t token_count
) {
    if (token_count == 0 || token_count > ScratchpadDMAMaximumJobs ||
        first_token_id > UINT32_MAX - (token_count - 1U)) {
        return false;
    }
    scratchpadDMARegister(0x10) = token_count;
    scratchpadDMARegister(0x14) = first_token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = ScratchpadDMACommandWaitBatch;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

// Delimit a compiler-certified DMA event tape. With macro execution disabled,
// both commands are no-ops and the enclosed submit/wait calls synchronize
// normally. With it enabled, QEMU records the same guest instruction stream
// and the explicit ordering of every submit and completion wait. SST replays
// that tape before resuming once at the end. The conservative instruction
// bound prevents a run from crossing an icount quantum; a device may silently
// retain the scalar path when the bound does not fit. Capture activation is
// intentionally not returned to the guest, so a host optimization decision
// cannot change guest control flow or modeled instruction timestamps.
inline bool globalDMAMacroBegin(
    uint64_t execution_id,
    uint32_t transfer_count,
    uint64_t maximum_instruction_span
) {
    if (transfer_count < 2 ||
        transfer_count > ScratchpadDMAMaximumJobs ||
        maximum_instruction_span == 0) {
        return false;
    }
    scratchpadDMARegister(0x00) =
        static_cast<uint32_t>(maximum_instruction_span);
    scratchpadDMARegister(0x04) =
        static_cast<uint32_t>(maximum_instruction_span >> 32);
    scratchpadDMARegister(0x10) = transfer_count;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = ScratchpadDMACommandMacroBegin;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

inline bool globalDMAMacroEnd(
    uint64_t execution_id,
    uint32_t transfer_count
) {
    if (transfer_count < 2 || transfer_count > ScratchpadDMAMaximumJobs) {
        return false;
    }
    scratchpadDMARegister(0x10) = transfer_count;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = ScratchpadDMACommandMacroEnd;
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}


}  // namespace golem::platform
