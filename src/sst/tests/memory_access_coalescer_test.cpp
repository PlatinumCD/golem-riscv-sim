#include "../memory/memoryAccessCoalescer.h"

#include <cassert>
#include <cstdint>
#include <vector>

using SST::Mittens::coalesceMemoryAccesses;
using SST::Mittens::canExtendScalarLoadGroup;

namespace {

MittensSyncMemoryAccess access(
    std::uint64_t instruction,
    std::uint64_t vectorInstruction,
    std::uint64_t address,
    std::uint64_t pc,
    std::uint32_t flags = MITTENS_SYNC_MEMORY_FLAG_NONE)
{
    return MittensSyncMemoryAccess{
        instruction,
        vectorInstruction,
        address,
        pc,
        UINT64_C(0x80001000),
        8,
        flags,
        0,
        0,
        0,
        1,
    };
}

} // namespace

int main()
{
    std::vector<MittensSyncMemoryAccess> vectorLoad;
    for (std::uint64_t element = 0; element < 16; ++element) {
        vectorLoad.push_back(access(
            100,
            7,
            UINT64_C(0x80002000) + element * 8,
            UINT64_C(0x80000200)));
    }
    const auto loadLines = coalesceMemoryAccesses(vectorLoad, 64);
    assert(loadLines.size() == 2);
    assert(loadLines[0].address == UINT64_C(0x80002000));
    assert(loadLines[0].size == 64);
    assert(loadLines[1].address == UINT64_C(0x80002040));
    assert(loadLines[1].size == 64);

    std::vector<MittensSyncMemoryAccess> vectorStore;
    for (std::uint64_t element = 0; element < 16; ++element) {
        vectorStore.push_back(access(
            101,
            8,
            UINT64_C(0x80003020) + element * 8,
            UINT64_C(0x8000020a),
            MITTENS_SYNC_MEMORY_FLAG_WRITE));
    }
    const auto storeLines = coalesceMemoryAccesses(vectorStore, 64);
    assert(storeLines.size() == 3);
    assert(storeLines[0].size == 32);
    assert(storeLines[1].size == 64);
    assert(storeLines[2].size == 32);

    auto separateInstructions = vectorLoad;
    separateInstructions[8].instructions_executed = 101;
    const auto separate = coalesceMemoryAccesses(
        separateInstructions, 64);
    assert(separate.size() == 3);
    assert(separate[0].size == 64);
    assert(separate[1].size == 8);
    assert(separate[2].size == 56);

    auto strided = vectorLoad;
    for (std::uint64_t element = 0; element < strided.size(); ++element) {
        strided[element].address =
            UINT64_C(0x80004000) + element * 16;
    }
    assert(coalesceMemoryAccesses(strided, 64).size() == 16);

    auto independentFirst = access(
        200, 10, UINT64_C(0x80005000), UINT64_C(0x80000300),
        MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS);
    independentFirst.source_register_mask = UINT32_C(1) << 10;
    independentFirst.destination_register_mask = UINT32_C(1) << 11;
    independentFirst.instruction_length = 4;
    auto independentSecond = access(
        201, 10, UINT64_C(0x80006000), UINT64_C(0x80000304),
        MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS);
    independentSecond.source_register_mask = UINT32_C(1) << 12;
    independentSecond.destination_register_mask = UINT32_C(1) << 13;
    assert(canExtendScalarLoadGroup(
        independentFirst,
        independentSecond,
        independentFirst.destination_register_mask));

    auto dependentSecond = independentSecond;
    dependentSecond.source_register_mask = UINT32_C(1) << 11;
    assert(!canExtendScalarLoadGroup(
        independentFirst,
        dependentSecond,
        independentFirst.destination_register_mask));

    auto separatedSecond = independentSecond;
    separatedSecond.program_counter += 4;
    assert(!canExtendScalarLoadGroup(
        independentFirst,
        separatedSecond,
        independentFirst.destination_register_mask));
    return 0;
}
