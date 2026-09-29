#include <stdint.h>
#define VECTOR_BYTES (VLEN_BITS / 8)
#define SOURCE ((uint8_t *)(uintptr_t)0x90101000)
#define SPILL(n) ((uint8_t *)(uintptr_t)(0x90160000 + (n) * 0x1000))
#define SHORT(n) ((uint8_t *)(uintptr_t)(0x90170000 + (n) * 0x1000))
extern void benchmark_marker_unfenced(unsigned long, unsigned long);

/* Two independent short loads must reserve their entire destination registers.
 * The dependent short stores check the active elements; full-register spills
 * after the marker independently expose tail handling. Store tails retain A5. */
#define PARTIAL_TEST(name, bits, policy, index) \
static __attribute__((noinline)) void name(void) { \
    const unsigned long full = VLEN_BITS / bits, short_vl = full - 1; \
    __asm__ volatile ( \
        "vsetvli zero, %0, e" #bits ", m1, tu, mu\n\t" \
        "vmv.v.i v8, 3\n\tvmv.v.i v9, 5\n\t" \
        "vsetvli zero, %1, e" #bits ", m1, " #policy ", mu" \
        : : "r"(full), "r"(short_vl) : "v8", "v9", "memory"); \
    benchmark_marker_unfenced(40 + index, 1); \
    __asm__ volatile ( \
        "vle" #bits ".v v8, (%0)\n\tvle" #bits ".v v9, (%1)\n\t" \
        "vse" #bits ".v v8, (%2)\n\tvse" #bits ".v v9, (%3)" \
        : : "r"(SOURCE), "r"(SOURCE + VECTOR_BYTES), \
            "r"(SHORT(index)), "r"(SHORT(index) + VECTOR_BYTES) \
        : "v8", "v9", "memory"); \
    benchmark_marker_unfenced(40 + index, 0); \
    __asm__ volatile ( \
        "vsetvli zero, %2, e" #bits ", m1, tu, mu\n\t" \
        "vse" #bits ".v v8, (%0)\n\tvse" #bits ".v v9, (%1)\n\tfence rw, rw" \
        : : "r"(SPILL(index)), "r"(SPILL(index) + VECTOR_BYTES), "r"(full) \
        : "memory"); \
}
PARTIAL_TEST(e8_tu, 8, tu, 0)
PARTIAL_TEST(e8_ta, 8, ta, 1)
PARTIAL_TEST(e16_tu, 16, tu, 2)
PARTIAL_TEST(e16_ta, 16, ta, 3)
PARTIAL_TEST(e32_tu, 32, tu, 4)
PARTIAL_TEST(e32_ta, 32, ta, 5)
PARTIAL_TEST(e64_tu, 64, tu, 6)
PARTIAL_TEST(e64_ta, 64, ta, 7)

int main(void) {
    for (unsigned i = 0; i < 2 * VECTOR_BYTES; ++i) SOURCE[i] = (13 * i + 7) & 255;
    for (unsigned phase = 0; phase < 8; ++phase)
        for (unsigned i = 0; i < 2 * VECTOR_BYTES; ++i) SHORT(phase)[i] = 0xa5;
    e8_tu(); e8_ta(); e16_tu(); e16_ta();
    e32_tu(); e32_ta(); e64_tu(); e64_ta();
    return 0;
}
