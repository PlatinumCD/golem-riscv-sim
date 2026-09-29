#pragma once
#include <cstdint>
#include <vector>

namespace TileComponents::Riscv {

struct InstructionCacheConfiguration {
    std::uint64_t capacityBytes = 8192;
    std::uint32_t lineBytes = 64, ways = 2;
    std::uint64_t hitLatencyCycles = 1;
};

struct InstructionCacheStatistics {
    std::uint64_t fetches = 0, hits = 0, misses = 0, fills = 0, fillBytes = 0;
    std::uint64_t evictions = 0, invalidations = 0, stallCycles = 0;
};

// Tag/LRU policy adapted from src/sst/memory/instructionCache. QEMU owns
// functional instruction execution; the CPU issues real StandardMem fills.
// A tag becomes resident only when all of its fill responses have completed.
class InstructionCache final {
public:
    InstructionCache(std::uint64_t spmBase, std::uint64_t spmBytes,
                     InstructionCacheConfiguration configuration);
    bool beginFetch(std::uint64_t address, std::uint32_t bytes);
    bool touch(std::uint64_t relativeLine);
    void fill(std::uint64_t relativeLine);
    void invalidate();
    void accountStall(std::uint64_t cycles) { statistics_.stallCycles += cycles; }
    const InstructionCacheConfiguration& configuration() const { return configuration_; }
    const InstructionCacheStatistics& statistics() const { return statistics_; }

private:
    struct Line { std::uint64_t tag = 0, age = 0; bool valid = false; };
    std::uint64_t spmBase_, spmBytes_, sets_, age_ = 0;
    InstructionCacheConfiguration configuration_;
    InstructionCacheStatistics statistics_;
    std::vector<Line> lines_;
    bool resident(std::uint64_t relativeLine) const;
};
}
