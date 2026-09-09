#include "../memory/addressRegion.h"
#include <mittens/MemoryMap.h>
#include <cassert>
#include <cstdint>
#include <iostream>

using SST::Mittens::AddressRegion;

int main() {
    constexpr AddressRegion spm{MITTENS_SCRATCHPAD_BASE, 16384};
    static_assert(spm.containsRange(MITTENS_SCRATCHPAD_BASE, 16384));
    static_assert(!spm.contains(MITTENS_SCRATCHPAD_BASE + 16384));
    static_assert(spm.containsRange(MITTENS_SCRATCHPAD_BASE + 16384, 0));
    static_assert(!spm.containsRange(MITTENS_SCRATCHPAD_BASE + 16384, 1));
    static_assert(!spm.containsRange(MITTENS_SCRATCHPAD_BASE - 1, 1));
    static_assert(!spm.crossesStart(MITTENS_SCRATCHPAD_BASE - 4, 4));
    static_assert(spm.crossesStart(MITTENS_SCRATCHPAD_BASE - 4, 5));
    static_assert(!AddressRegion::representable(UINT64_MAX, 1));
    static_assert(AddressRegion::representable(UINT64_MAX, 0));
    static_assert(AddressRegion{0, UINT64_MAX}.containsRange(0, UINT64_MAX));
    static_assert(!AddressRegion{0, UINT64_MAX}.containsRange(UINT64_MAX - 3, 4));
    constexpr AddressRegion empty{100, 0};
    static_assert(empty.containsRange(100, 0) && !empty.contains(100));
    assert(spm.offset(MITTENS_SCRATCHPAD_BASE + 32, 32) == 32);
    bool rejected = false;
    try { (void)spm.offset(MITTENS_SCRATCHPAD_BASE - 1, 1); }
    catch (const std::out_of_range&) { rejected = true; }
    assert(rejected);
    // Exhaustively compare every small interval to its straightforward oracle.
    for (std::uint64_t base = 0; base < 8; ++base)
        for (std::uint64_t size = 0; size < 16; ++size)
            for (std::uint64_t addr = 0; addr < 32; ++addr)
                for (std::uint64_t len = 0; len < 32; ++len) {
                    const bool expected = addr >= base && addr <= base + size && addr + len <= base + size;
                    assert((AddressRegion{base, size}.containsRange(addr, len)) == expected);
                }
    std::cout << "memory address regions: PASS\n";
}
