#include "instructionCache.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using TileComponents::Riscv::InstructionCache;
using TileComponents::Riscv::InstructionCacheConfiguration;

namespace {
constexpr std::uint64_t Base = 0x1000;
constexpr std::uint64_t Capacity = 4096;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Action>
void rejects(Action action, const char* message) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void lruAndInvalidation() {
    InstructionCache cache(Base, Capacity, {128, 64, 2, 1});
    require(!cache.beginFetch(Base, 4), "cold A must miss");
    cache.fill(0);
    cache.fill(64);
    require(cache.touch(0), "A must be resident before its LRU touch");
    cache.fill(128);
    // FIFO would evict A here. Refreshing A must instead evict B.
    require(!cache.beginFetch(Base + 64, 4), "LRU must evict B after touching A");
    require(cache.beginFetch(Base, 4), "recently touched A must survive eviction");
    require(cache.beginFetch(Base + 128, 4), "new C must be resident");
    require(cache.statistics().evictions == 1, "one insertion needs one eviction");
    cache.invalidate();
    require(!cache.beginFetch(Base, 4) && !cache.beginFetch(Base + 128, 4),
            "invalidation must remove every resident line");
    require(cache.statistics().invalidations == 1, "invalidation must be counted");
}

void crossingFetches() {
    InstructionCache cache(Base, Capacity, {128, 64, 2, 1});
    require(!cache.beginFetch(Base + 62, 4), "crossing with two absent lines must miss");
    cache.fill(0);
    require(!cache.beginFetch(Base + 62, 4), "crossing with absent second line must miss");
    cache.fill(64);
    require(cache.beginFetch(Base + 62, 4), "crossing hits only when both lines reside");
    require(cache.statistics().fetches == 3 && cache.statistics().misses == 2 &&
            cache.statistics().hits == 1 && cache.statistics().fills == 2,
            "crossing counts one fetch outcome and one fill per line");
    cache.invalidate();
    cache.fill(64);
    require(!cache.beginFetch(Base + 62, 4), "crossing with absent first line must miss");

    InstructionCache small(Base, Capacity, {8, 4, 2, 1});
    require(!small.beginFetch(Base + 2, 4), "small-line crossing must initially miss");
    small.fill(0);
    small.fill(4);
    require(small.beginFetch(Base + 2, 4), "four-byte cache lines can hold a crossing");
}

void invalidInputs() {
    const auto badGeometry = [](InstructionCacheConfiguration configuration,
                                std::uint64_t base = Base, std::uint64_t bytes = Capacity) {
        rejects<std::invalid_argument>([&] { InstructionCache cache(base, bytes, configuration); },
                                       "invalid cache geometry was accepted");
    };
    badGeometry({0, 64, 2, 1});
    badGeometry({128, 0, 2, 1});
    badGeometry({128, 3, 2, 1});
    badGeometry({128, 48, 2, 1});
    badGeometry({64, 64, 2, 1});
    badGeometry({128, 64, 0, 1});
    badGeometry({256, 64, 3, 1});
    badGeometry({128, 64, 2, 0});
    badGeometry({128, 64, 2, 1}, Base + 1);
    badGeometry({128, 64, 2, 1}, Base, 32);
    badGeometry({128, 64, 2, 1}, Base, 96);
    badGeometry({128, 64, 2, 1}, std::numeric_limits<std::uint64_t>::max() - 63);

    InstructionCache cache(Base, Capacity, {});
    cache.fill(0);
    const auto before = cache.statistics();
    for (const auto bytes : {0u, 1u, 3u, 8u}) {
        rejects<std::out_of_range>([&] { cache.beginFetch(Base, bytes); },
                                   "invalid instruction size was accepted");
    }
    for (const auto address : {Base - 2, Base + 1, Base + Capacity - 2, Base + Capacity}) {
        rejects<std::out_of_range>([&] { cache.beginFetch(address, 4); },
                                   "invalid instruction address was accepted");
    }
    require(cache.statistics().fetches == before.fetches &&
            cache.statistics().hits == before.hits && cache.statistics().misses == before.misses,
            "rejected fetches must leave fetch accounting unchanged");
    require(cache.beginFetch(Base, 4), "rejected fetches must preserve resident data");
}
}

int main() {
    try {
        lruAndInvalidation();
        crossingFetches();
        invalidInputs();
        std::cout << "PASS instruction-cache native policy\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL instruction-cache native policy: " << error.what() << '\n';
        return 1;
    }
}
