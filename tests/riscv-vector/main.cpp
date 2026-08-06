#include <stdint.h>

#include "platform.h"

namespace {

constexpr uint32_t kElementCount = 8;

alignas(32) float left[kElementCount] = {
    1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F,
};
alignas(32) float right[kElementCount] = {
    0.5F, -1.0F, 1.5F, -2.0F, 2.5F, -3.0F, 3.5F, -4.0F,
};
alignas(32) float result[kElementCount] = {};

bool runVectorAdd()
{
    unsigned long vectorLengthBytes = 0;
    unsigned long activeElements = 0;
    const unsigned long requestedElements = kElementCount;

    asm volatile(
        "csrr %0, vlenb\n\t"
        "vsetvli %1, %5, e32, m1, ta, ma\n\t"
        "vle32.v v8, (%2)\n\t"
        "vle32.v v9, (%3)\n\t"
        "vfadd.vv v10, v8, v9\n\t"
        "vse32.v v10, (%4)"
        : "=&r"(vectorLengthBytes), "=&r"(activeElements)
        : "r"(left), "r"(right), "r"(result), "r"(requestedElements)
        : "v8", "v9", "v10", "memory");

    if (vectorLengthBytes != 32 ||
        activeElements != kElementCount) {
        return false;
    }

    constexpr float expected[kElementCount] = {
        1.5F, 1.0F, 4.5F, 2.0F, 7.5F, 3.0F, 10.5F, 4.0F,
    };
    for (uint32_t index = 0; index < kElementCount; ++index) {
        if (result[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" int tile_main()
{
    if (!runVectorAdd()) {
        uart_puts("RISCV_VECTOR_FAIL\n");
        return 1;
    }

    uart_puts(
        "RISCV_VECTOR_PASS: RVV 1.0 VLEN=256 floating-point vector add\n");
    return 0;
}
