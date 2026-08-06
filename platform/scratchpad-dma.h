#pragma once

#include <stddef.h>
#include <stdint.h>

namespace golem::platform {

constexpr uintptr_t ScratchpadBase = UINT64_C(0x90000000);
constexpr uintptr_t ScratchpadDMARegisterBase = UINT64_C(0x10011000);

enum class ScratchpadDMADirection : uint32_t {
    BackingToScratchpad = 0,
    ScratchpadToBacking = 1,
    NicToScratchpad = 2,
    ScratchpadToNic = 3,
};

inline volatile uint32_t& scratchpadDMARegister(uintptr_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(
        ScratchpadDMARegisterBase + offset);
}

inline void scratchpadDMASubmit(
    const void* source,
    void* destination,
    uint32_t byte_count,
    uint32_t token_id,
    uint64_t execution_id,
    ScratchpadDMADirection direction
) {
    const uint64_t source_address =
        reinterpret_cast<uintptr_t>(source);
    const uint64_t destination_address =
        reinterpret_cast<uintptr_t>(destination);
    scratchpadDMARegister(0x00) = static_cast<uint32_t>(source_address);
    scratchpadDMARegister(0x04) = static_cast<uint32_t>(source_address >> 32);
    scratchpadDMARegister(0x08) = static_cast<uint32_t>(destination_address);
    scratchpadDMARegister(0x0c) = static_cast<uint32_t>(destination_address >> 32);
    scratchpadDMARegister(0x10) = byte_count;
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x20) = static_cast<uint32_t>(direction);
    scratchpadDMARegister(0x24) = 1;
}

inline bool scratchpadDMAWait(uint64_t execution_id, uint32_t token_id) {
    scratchpadDMARegister(0x14) = token_id;
    scratchpadDMARegister(0x18) = static_cast<uint32_t>(execution_id);
    scratchpadDMARegister(0x1c) = static_cast<uint32_t>(execution_id >> 32);
    scratchpadDMARegister(0x24) = 2;
    return (scratchpadDMARegister(0x28) & (UINT32_C(1) << 2)) == 0;
}

}  // namespace golem::platform
