#include <stdint.h>
#define VB (VLEN_BITS / 8)
#define SOURCE ((uint8_t *)(uintptr_t)0x90101000)
#define CROSS_SOURCE ((uint8_t *)(uintptr_t)0x90102ff0)
#define OUTPUT(n) ((uint8_t *)(uintptr_t)(0x90110000 + (n) * 0x1000))
#define CROSS_OUTPUT ((uint8_t *)(uintptr_t)0x9014fff0)
#define CONTROL ((volatile uint64_t *)(uintptr_t)0x90160000)
#define CLOBBERS "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", \
    "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", \
    "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory"
extern void benchmark_marker_unfenced(unsigned long, unsigned long);
#define MARK(n, start) benchmark_marker_unfenced(100 + (n), start)

/* Whole-register instructions ignore VL and vtype, including vill. Widths
 * 8/32 use VL=0 with otherwise legal e64,m8; widths 16/64 use vill and VL=0. */
#define WHOLE(name, count, bits, index, illegal) \
static __attribute__((noinline)) void name(void) { \
    if (illegal) { \
        __asm__ volatile ("li t0, 0x100\n\tvsetvl zero, zero, t0" : : : "t0", "memory"); \
    } else { \
        __asm__ volatile ("vsetivli zero, 0, e64, m8, tu, mu" : : : "memory"); \
    } \
    MARK(index, 1); \
    __asm__ volatile ("vl" #count "re" #bits ".v v8, (%0)\n\tvs" #count "r.v v8, (%1)" \
        : : "r"(SOURCE), "r"(OUTPUT(index)) : CLOBBERS); \
    MARK(index, 0); \
    unsigned long vl, vtype; \
    __asm__ volatile ("csrr %0, vl\n\tcsrr %1, vtype" : "=r"(vl), "=r"(vtype)); \
    CONTROL[2 * (index)] = vl; CONTROL[2 * (index) + 1] = vtype; \
}
#define WHOLE_WIDTH(bits, base, illegal) \
WHOLE(whole1_##bits, 1, bits, base, illegal) \
WHOLE(whole2_##bits, 2, bits, base + 1, illegal) \
WHOLE(whole4_##bits, 4, bits, base + 2, illegal) \
WHOLE(whole8_##bits, 8, bits, base + 3, illegal)
WHOLE_WIDTH(8, 0, 0)
WHOLE_WIDTH(16, 4, 1)
WHOLE_WIDTH(32, 8, 0)
WHOLE_WIDTH(64, 12, 1)

/* Two independent LMUL groups exercise byte accounting, dependencies through
 * each physical register, and a tail spanning wholly inactive registers when
 * tiny=1. Full spills expose tails; short stores must leave their guards alone. */
#define PARTIAL(name, group, bits, policy, index, tiny) \
static __attribute__((noinline)) void name(void) { \
    unsigned long full = group * VLEN_BITS / bits; \
    unsigned long active = tiny ? 1 : full - 1; \
    __asm__ volatile ("vsetvli zero, %0, e" #bits ", m" #group ", tu, mu\n\t" \
        "vmv.v.i v8, 3\n\tvmv.v.i v16, 5\n\t" \
        "vsetvli zero, %1, e" #bits ", m" #group ", " #policy ", mu" \
        : : "r"(full), "r"(active) : CLOBBERS); \
    MARK(index, 1); \
    __asm__ volatile ("vle" #bits ".v v8, (%0)\n\tvle" #bits ".v v16, (%1)\n\t" \
        "vse" #bits ".v v8, (%2)\n\tvse" #bits ".v v16, (%3)" \
        : : "r"(SOURCE), "r"(SOURCE + group * VB), \
            "r"(OUTPUT(index)), "r"(OUTPUT(index) + group * VB) : CLOBBERS); \
    MARK(index, 0); \
    __asm__ volatile ("vs" #group "r.v v8, (%0)\n\tvs" #group "r.v v16, (%1)\n\tfence rw, rw" \
        : : "r"(OUTPUT(index) + 0x800), "r"(OUTPUT(index) + 0x800 + group * VB) : "memory"); \
}
#define PARTIAL_WIDTH(group, bits, base) \
PARTIAL(partial_##group##_##bits##_tu, group, bits, tu, base, 0) \
PARTIAL(partial_##group##_##bits##_ta, group, bits, ta, base + 1, 0)
#define PARTIAL_GROUP(group, base) \
PARTIAL_WIDTH(group, 8, base) \
PARTIAL_WIDTH(group, 16, base + 2) \
PARTIAL_WIDTH(group, 32, base + 4) \
PARTIAL_WIDTH(group, 64, base + 6)
PARTIAL_GROUP(2, 16)
PARTIAL_GROUP(4, 24)
PARTIAL_GROUP(8, 32)
PARTIAL(tiny_tu, 8, 32, tu, 40, 1)
PARTIAL(tiny_ta, 8, 32, ta, 41, 1)

static void hazards(void) {
    __asm__ volatile ("vsetvli zero, %0, e8, m1, tu, mu" : : "r"((unsigned long)VB) : "memory");
    MARK(42, 1);
    __asm__ volatile (".balign 64\n\tvl8re8.v v8, (%0)\n\tvadd.vi v24, v15, 1\n\tvs1r.v v24, (%1)"
        : : "r"(SOURCE), "r"(OUTPUT(42)) : CLOBBERS);
    MARK(42, 0);
    MARK(43, 1);
    __asm__ volatile (".balign 64\n\tvl8re8.v v8, (%0)\n\tvl1re8.v v15, (%1)\n\tvs8r.v v8, (%2)"
        : : "r"(SOURCE), "r"(SOURCE + 8 * VB), "r"(OUTPUT(43)) : CLOBBERS);
    MARK(43, 0);
    __asm__ volatile ("vl8re8.v v8, (%0)\n\tfence rw, rw" : : "r"(SOURCE) : CLOBBERS);
    MARK(44, 1);
    __asm__ volatile ("vs8r.v v8, (%0)\n\tvmv.v.i v15, 7\n\tvs1r.v v15, (%1)"
        : : "r"(OUTPUT(44)), "r"(OUTPUT(44) + 8 * VB) : CLOBBERS);
    MARK(44, 0);
}

static void fallbacks(void) {
    __asm__ volatile ("vsetivli zero, 0, e64, m8, tu, mu" : : : "memory");
    MARK(45, 1);
    __asm__ volatile ("vl8re32.v v8, (%0)\n\tvs8r.v v8, (%1)"
        : : "r"(CROSS_SOURCE), "r"(OUTPUT(45)) : CLOBBERS);
    MARK(45, 0);
    MARK(46, 1);
    __asm__ volatile ("vl8re32.v v8, (%0)\n\tvs8r.v v8, (%1)"
        : : "r"(SOURCE), "r"(CROSS_OUTPUT) : CLOBBERS);
    MARK(46, 0);
    __asm__ volatile ("vl8re8.v v8, (%0)\n\tfence rw, rw" : : "r"(SOURCE) : CLOBBERS);
    MARK(47, 1);
    __asm__ volatile ("csrwi vstart, 1\n\tvl8re32.v v8, (%0)\n\tcsrwi vstart, 1\n\tvs8r.v v8, (%1)"
        : : "r"(SOURCE + 8 * VB), "r"(OUTPUT(47)) : CLOBBERS);
    MARK(47, 0);
    __asm__ volatile ("vsetvli zero, %0, e32, m8, tu, mu\n\tvmv.v.i v8, 3"
        : : "r"((unsigned long)(8 * VLEN_BITS / 32)) : CLOBBERS);
    MARK(48, 1);
    __asm__ volatile ("csrwi vstart, 1\n\tvle32.v v8, (%0)\n\tcsrwi vstart, 1\n\tvse32.v v8, (%1)"
        : : "r"(SOURCE), "r"(OUTPUT(48)) : CLOBBERS);
    MARK(48, 0);
}

static void reconfigure(void) {
    unsigned long full = 8 * VLEN_BITS / 32, active = VLEN_BITS / 32 + 1;
    __asm__ volatile ("vsetvli zero, %0, e32, m8, tu, mu\n\tvmv.v.i v8, 3\n\t"
        "vsetvli zero, %1, e32, m8, ta, mu" : : "r"(full), "r"(active) : CLOBBERS);
    MARK(49, 1);
    __asm__ volatile (".balign 64\n\tvle32.v v8, (%0)\n\tvsetvli zero, %2, e8, m1, tu, mu\n\t"
        "vmv.v.i v15, 7\n\tvs8r.v v8, (%1)"
        : : "r"(SOURCE), "r"(OUTPUT(49)), "r"((unsigned long)VB) : CLOBBERS);
    MARK(49, 0);
    __asm__ volatile ("vsetvli zero, %0, e32, m8, tu, mu" : : "r"(full) : "memory");
    MARK(50, 1);
    __asm__ volatile (".balign 64\n\tvle32.v v8, (%0)\n\tvle32.v v16, (%1)\n\t"
        "vadd.vv v24, v8, v16\n\tvse32.v v24, (%2)"
        : : "r"(SOURCE), "r"(SOURCE + 8 * VB), "r"(OUTPUT(50)) : CLOBBERS);
    MARK(50, 0);
}

int main(void) {
    for (unsigned i = 0; i < 16 * VB; ++i) SOURCE[i] = (13 * i + (i >> 8) * 11 + 7) & 255;
    for (unsigned i = 0; i < 8 * VB; ++i) CROSS_SOURCE[i] = (17 * i + 9) & 255;
    for (unsigned phase = 0; phase < 51; ++phase)
        for (unsigned i = 0; i < 16 * VB; ++i) OUTPUT(phase)[i] = 0xa5;
    for (unsigned i = 0; i < 8 * VB + 16; ++i) CROSS_OUTPUT[i] = 0xa5;
#define CALL_WHOLE(bits) whole1_##bits(); whole2_##bits(); whole4_##bits(); whole8_##bits()
    CALL_WHOLE(8); CALL_WHOLE(16); CALL_WHOLE(32); CALL_WHOLE(64);
#define CALL_WIDTH(group, bits) partial_##group##_##bits##_tu(); partial_##group##_##bits##_ta()
#define CALL_GROUP(group) CALL_WIDTH(group, 8); CALL_WIDTH(group, 16); CALL_WIDTH(group, 32); CALL_WIDTH(group, 64)
    CALL_GROUP(2); CALL_GROUP(4); CALL_GROUP(8);
    tiny_tu(); tiny_ta(); hazards(); fallbacks(); reconfigure();
    return 0;
}
