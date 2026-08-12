#include <stdint.h>

#include "mesh-nic.h"

#if !defined(MITTENS_MEMORY_TEST_CONFLICT) && \
    !defined(MITTENS_MEMORY_TEST_CAPACITY) && \
    !defined(MITTENS_MEMORY_TEST_STORE_BUFFER) && \
    !defined(MITTENS_MEMORY_TEST_VECTOR_GROUP) && \
    !defined(MITTENS_MEMORY_TEST_SCALAR_INDEPENDENT) && \
    !defined(MITTENS_MEMORY_TEST_SCALAR_DEPENDENT)
#error "select one memory timing validation case"
#endif

namespace {

constexpr uintptr_t kLineBytes = 64;
constexpr uintptr_t kSetSpanBytes = 128 * kLineBytes;

#if defined(MITTENS_MEMORY_TEST_CONFLICT)
alignas(kSetSpanBytes) uint8_t probeMemory[4 * kSetSpanBytes + 8] = {};
#elif defined(MITTENS_MEMORY_TEST_CAPACITY)
alignas(kSetSpanBytes) uint8_t probeMemory[513 * kLineBytes] = {};
#elif defined(MITTENS_MEMORY_TEST_VECTOR_GROUP)
alignas(kLineBytes) float vectorProbe[24] = {
    1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F,
    9.0F, 10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F,
    17.0F, 18.0F, 19.0F, 20.0F, 21.0F, 22.0F, 23.0F, 24.0F,
};
alignas(kLineBytes) float vectorResult[24] = {};
#elif defined(MITTENS_MEMORY_TEST_SCALAR_INDEPENDENT) || \
      defined(MITTENS_MEMORY_TEST_SCALAR_DEPENDENT)
alignas(kLineBytes) volatile uintptr_t scalarProbe[32] = {};
#else
alignas(kLineBytes) volatile uint64_t storeProbe[64] = {};
#endif

}  // namespace

extern "C" int tile_main() {
#if defined(MITTENS_MEMORY_TEST_SCALAR_INDEPENDENT)
    scalarProbe[0] = 17;
    scalarProbe[16] = 25;
#elif defined(MITTENS_MEMORY_TEST_SCALAR_DEPENDENT)
    scalarProbe[0] = reinterpret_cast<uintptr_t>(&scalarProbe[16]);
    scalarProbe[16] = 42;
#endif
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
#elif defined(MITTENS_MEMORY_TEST_STORE_BUFFER)
    for (uint64_t index = 0; index < 64; ++index) {
        storeProbe[index] = index + 1;
    }
    asm volatile("fence rw, rw" ::: "memory");
    uint64_t sum = 0;
    for (uint64_t index = 0; index < 64; ++index) {
        sum += storeProbe[index];
    }
    return sum == (64 * 65) / 2 ? 0 : 1;
#elif defined(MITTENS_MEMORY_TEST_VECTOR_GROUP)
    const unsigned long elementCount = 8;
    const float* input = &vectorProbe[12];
    float* output = &vectorResult[12];
    asm volatile(
        "vsetvli zero, %2, e32, m1, ta, ma\n\t"
        "vle32.v v8, (%0)\n\t"
        "vse32.v v8, (%1)"
        :
        : "r"(input), "r"(output), "r"(elementCount)
        : "v8", "memory");
    for (uint32_t index = 0; index < elementCount; ++index) {
        if (output[index] != input[index]) {
            return 1;
        }
    }
    return 0;
#elif defined(MITTENS_MEMORY_TEST_SCALAR_INDEPENDENT)
    const uintptr_t first = reinterpret_cast<uintptr_t>(&scalarProbe[0]);
    const uintptr_t second = reinterpret_cast<uintptr_t>(&scalarProbe[16]);
    uintptr_t firstValue;
    uintptr_t secondValue;
    asm volatile(
        "ld %0, 0(%2)\n\t"
        "ld %1, 0(%3)"
        : "=&r"(firstValue), "=&r"(secondValue)
        : "r"(first), "r"(second)
        : "memory");
    return firstValue == 17 && secondValue == 25 ? 0 : 1;
#else
    const uintptr_t base = reinterpret_cast<uintptr_t>(&scalarProbe[0]);
    uintptr_t target;
    uintptr_t value;
    asm volatile(
        "ld %0, 0(%2)\n\t"
        "ld %1, 0(%0)"
        : "=&r"(target), "=&r"(value)
        : "r"(base)
        : "memory");
    return target == reinterpret_cast<uintptr_t>(&scalarProbe[16]) &&
                   value == 42
               ? 0
               : 1;
#endif
}
