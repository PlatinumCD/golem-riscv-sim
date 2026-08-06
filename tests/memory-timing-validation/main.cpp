#include <stdint.h>

#include "mesh-nic.h"

#if !defined(MITTENS_MEMORY_TEST_CONFLICT) && \
    !defined(MITTENS_MEMORY_TEST_CAPACITY) && \
    !defined(MITTENS_MEMORY_TEST_STORE_BUFFER)
#error "select one memory timing validation case"
#endif

namespace {

constexpr uintptr_t kLineBytes = 64;
constexpr uintptr_t kSetSpanBytes = 128 * kLineBytes;

#if defined(MITTENS_MEMORY_TEST_CONFLICT)
alignas(kSetSpanBytes) uint8_t probeMemory[4 * kSetSpanBytes + 8] = {};
#elif defined(MITTENS_MEMORY_TEST_CAPACITY)
alignas(kSetSpanBytes) uint8_t probeMemory[513 * kLineBytes] = {};
#else
alignas(kLineBytes) volatile uint64_t storeProbe[64] = {};
#endif

}  // namespace

extern "C" int tile_main() {
    /*
     * All CRT and image-initialization traffic is aggregated before this
     * marker and does not populate the L1. The accesses below therefore
     * begin from a known cold cache.
     */
    mesh_nic::complete_memory_initialization();

#if defined(MITTENS_MEMORY_TEST_CONFLICT)
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(probeMemory);
    const uintptr_t lineA = base;
    const uintptr_t lineB = base + kSetSpanBytes;
    const uintptr_t lineC = base + 2 * kSetSpanBytes;
    const uintptr_t lineD = base + 3 * kSetSpanBytes;
    const uintptr_t lineE = base + 4 * kSetSpanBytes;
    uint64_t finalA;

    /*
     * A-E all map to one set in the four-way L1.
     *
     * Expected classifications:
     *   A miss, A hit, A write hit, B/C/D miss, A hit,
     *   E miss evicting B, B miss, A hit.
     */
    asm volatile(
        "ld t0, 0(%1)\n\t"
        "ld t0, 0(%1)\n\t"
        "li t1, 7\n\t"
        "sd t1, 0(%1)\n\t"
        "ld t0, 0(%2)\n\t"
        "ld t0, 0(%3)\n\t"
        "ld t0, 0(%4)\n\t"
        "ld t0, 0(%1)\n\t"
        "ld t0, 0(%5)\n\t"
        "ld t0, 0(%2)\n\t"
        "ld %0, 0(%1)"
        : "=&r"(finalA)
        : "r"(lineA), "r"(lineB), "r"(lineC), "r"(lineD), "r"(lineE)
        : "t0", "t1", "memory");

    return finalA == 7 ? 0 : 1;
#elif defined(MITTENS_MEMORY_TEST_CAPACITY)
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(probeMemory);
    const uintptr_t end = base + 512 * kLineBytes;
    const uintptr_t evicted = base + 128 * kLineBytes;
    uint64_t finalValue;

    /*
     * Fill all 512 lines, refresh line 0, then cross the 32 KiB capacity.
     * Line 512 maps to set zero and evicts line 128 under LRU. The final
     * line-128 read must therefore miss.
     */
    asm volatile(
        "mv t0, %1\n"
        "1:\n\t"
        "ld t1, 0(t0)\n\t"
        "addi t0, t0, 64\n\t"
        "bne t0, %2, 1b\n\t"
        "ld t1, 0(%1)\n\t"
        "ld t1, 0(%2)\n\t"
        "ld %0, 0(%3)"
        : "=&r"(finalValue)
        : "r"(base), "r"(end), "r"(evicted)
        : "t0", "t1", "memory");

    return finalValue == 0 ? 0 : 1;
#else
    for (uint64_t index = 0; index < 64; ++index) {
        storeProbe[index] = index + 1;
    }
    uint64_t sum = 0;
    for (uint64_t index = 0; index < 64; ++index) {
        sum += storeProbe[index];
    }
    return sum == (64 * 65) / 2 ? 0 : 1;
#endif
}
