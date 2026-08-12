#ifndef SST_MITTENS_MEMORY_ACCESS_COALESCER_H
#define SST_MITTENS_MEMORY_ACCESS_COALESCER_H

#include "mittens/SyncTileBridge.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace SST {
namespace Mittens {

inline bool sameDynamicMemoryInstruction(
    const MittensSyncMemoryAccess& left,
    const MittensSyncMemoryAccess& right) noexcept
{
    return left.instructions_executed == right.instructions_executed &&
           left.vector_instructions_executed ==
               right.vector_instructions_executed &&
           left.program_counter == right.program_counter &&
           left.return_address == right.return_address &&
           left.flags == right.flags;
}

inline bool canExtendScalarLoadGroup(
    const MittensSyncMemoryAccess& previous,
    const MittensSyncMemoryAccess& next,
    std::uint32_t producedRegisterMask) noexcept
{
    const std::uint32_t requiredFlags =
        MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS;
    const std::uint32_t disallowedFlags =
        MITTENS_SYNC_MEMORY_FLAG_WRITE |
        MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD;

    if ((previous.flags & requiredFlags) == 0 ||
        (next.flags & requiredFlags) == 0 ||
        (previous.flags & disallowedFlags) != 0 ||
        (next.flags & disallowedFlags) != 0) {
        return false;
    }
    if ((previous.instruction_length != 2 &&
         previous.instruction_length != 4) ||
        previous.program_counter >
            UINT64_MAX - previous.instruction_length ||
        next.program_counter !=
            previous.program_counter + previous.instruction_length ||
        next.vector_instructions_executed !=
            previous.vector_instructions_executed) {
        return false;
    }
    return (next.source_register_mask & producedRegisterMask) == 0;
}

/*
 * QEMU reports one record for each element touched by an RVV memory
 * instruction. MemHierarchy must see the cache-line fragments of the
 * architectural instruction, not a blocking request for every element.
 */
inline std::vector<MittensSyncMemoryAccess> coalesceMemoryAccesses(
    const std::vector<MittensSyncMemoryAccess>& accesses,
    std::uint32_t cacheLineSize)
{
    std::vector<MittensSyncMemoryAccess> result;
    result.reserve(accesses.size());

    for (const MittensSyncMemoryAccess& access : accesses) {
        if (!result.empty()) {
            MittensSyncMemoryAccess& previous = result.back();
            const std::uint64_t previousEnd =
                previous.address + previous.size;
            const std::uint64_t previousLine =
                previous.address / cacheLineSize;
            const std::uint64_t accessLine =
                access.address / cacheLineSize;
            const bool sizeFits =
                previous.size <=
                std::numeric_limits<std::uint32_t>::max() - access.size;

            if (sameDynamicMemoryInstruction(previous, access) &&
                previousEnd >= previous.address &&
                previousEnd == access.address &&
                previousLine == accessLine &&
                sizeFits) {
                previous.size += access.size;
                continue;
            }
        }
        result.push_back(access);
    }
    return result;
}

} // namespace Mittens
} // namespace SST

#endif
