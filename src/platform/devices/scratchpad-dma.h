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
constexpr uint32_t ScratchpadDMACommandInitializeGlobalRAM = 3;
constexpr uint32_t ScratchpadDMACommandWaitBatch = 4;
constexpr uint32_t ScratchpadDMACommandMacroBegin = 5;
constexpr uint32_t ScratchpadDMACommandMacroEnd = 6;
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

// Populate the functional global-RAM backing during deployment
// initialization. The initialization marker charges the aggregate byte
// volume once, and QEMU rejects this command after that marker.
inline bool globalRAMInitialize(
    uint64_t global_offset,
    uint64_t scratchpad_offset,
    uint32_t byte_count
) {
    if ((scratchpadDMARegister(0x28) & ScratchpadDMAStatusReady) == 0) {
        return false;
    }
    scratchpadDMARegister(0x00) = static_cast<uint32_t>(global_offset);
    scratchpadDMARegister(0x04) = static_cast<uint32_t>(global_offset >> 32);
    scratchpadDMARegister(0x08) = static_cast<uint32_t>(scratchpad_offset);
    scratchpadDMARegister(0x0c) =
        static_cast<uint32_t>(scratchpad_offset >> 32);
    scratchpadDMARegister(0x10) = byte_count;
    scratchpadDMARegister(0x20) = static_cast<uint32_t>(
        ScratchpadDMADirection::ScratchpadToGlobalRAM);
    __asm__ volatile("fence rw, iorw" ::: "memory");
    scratchpadDMARegister(0x24) =
        ScratchpadDMACommandInitializeGlobalRAM;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return (scratchpadDMARegister(0x28) & ScratchpadDMAStatusError) == 0;
}

}  // namespace golem::platform
