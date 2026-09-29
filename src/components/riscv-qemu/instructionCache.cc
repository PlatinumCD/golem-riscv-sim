#include "instructionCache.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace TileComponents::Riscv {
namespace {
bool powerOfTwo(std::uint64_t value) { return value && !(value & (value - 1)); }
}

InstructionCache::InstructionCache(std::uint64_t spmBase, std::uint64_t spmBytes,
                                   InstructionCacheConfiguration configuration)
    : spmBase_(spmBase), spmBytes_(spmBytes), configuration_(configuration) {
    const auto& c = configuration_;
    const auto span = std::uint64_t(c.lineBytes) * c.ways;
    if (!powerOfTwo(c.capacityBytes) || c.capacityBytes > 16 * 1024 * 1024 ||
        !powerOfTwo(c.lineBytes) || c.lineBytes < 4 || !powerOfTwo(c.ways) ||
        c.capacityBytes < span || !c.hitLatencyCycles || c.hitLatencyCycles > INT32_MAX ||
        c.lineBytes > spmBytes_ || spmBytes_ % c.lineBytes || spmBase_ % c.lineBytes ||
        spmBytes_ > std::numeric_limits<std::uint64_t>::max() - spmBase_)
        throw std::invalid_argument("invalid instruction cache geometry or hit latency");
    sets_ = c.capacityBytes / span;
    lines_.resize(c.capacityBytes / c.lineBytes);
}

bool InstructionCache::resident(std::uint64_t relativeLine) const {
    const auto number = relativeLine / configuration_.lineBytes;
    const auto begin = lines_.begin() + (number % sets_) * configuration_.ways;
    return std::any_of(begin, begin + configuration_.ways, [=](const Line& line) {
        return line.valid && line.tag == number / sets_;
    });
}

bool InstructionCache::beginFetch(std::uint64_t address, std::uint32_t bytes) {
    if ((bytes != 2 && bytes != 4) || (address & 1) || address < spmBase_ ||
        address - spmBase_ >= spmBytes_ || bytes > spmBytes_ - (address - spmBase_))
        throw std::out_of_range("instruction fetch is invalid or outside local SPM");
    const auto first = (address - spmBase_) & ~(std::uint64_t(configuration_.lineBytes) - 1);
    const auto last = (address - spmBase_ + bytes - 1) & ~(std::uint64_t(configuration_.lineBytes) - 1);
    const bool hit = resident(first) && resident(last);
    ++statistics_.fetches;
    ++(hit ? statistics_.hits : statistics_.misses);
    return hit;
}

bool InstructionCache::touch(std::uint64_t relativeLine) {
    const auto number = relativeLine / configuration_.lineBytes;
    auto begin = lines_.begin() + (number % sets_) * configuration_.ways;
    const auto end = begin + configuration_.ways;
    const auto found = std::find_if(begin, end, [=](const Line& line) {
        return line.valid && line.tag == number / sets_;
    });
    if (found == end) return false;
    found->age = ++age_;
    return true;
}

void InstructionCache::fill(std::uint64_t relativeLine) {
    if (relativeLine % configuration_.lineBytes || relativeLine >= spmBytes_ || resident(relativeLine))
        throw std::logic_error("invalid or duplicate instruction cache fill");
    const auto number = relativeLine / configuration_.lineBytes;
    auto begin = lines_.begin() + (number % sets_) * configuration_.ways;
    const auto end = begin + configuration_.ways;
    auto victim = std::find_if(begin, end, [](const Line& line) { return !line.valid; });
    if (victim == end) {
        victim = std::min_element(begin, end, [](const Line& a, const Line& b) { return a.age < b.age; });
        ++statistics_.evictions;
    }
    *victim = {number / sets_, ++age_, true};
    ++statistics_.fills;
    statistics_.fillBytes += configuration_.lineBytes;
}

void InstructionCache::invalidate() {
    for (auto& line : lines_) line.valid = false;
    ++statistics_.invalidations;
}
}
