#include <stdint.h>

#ifndef SEW
#define SEW 32
#endif
#ifndef LMUL
#define LMUL m1
#endif
#define STRING_(x) #x
#define STRING(x) STRING_(x)
#define VTYPE "e" STRING(SEW) ", " STRING(LMUL) ", tu, mu\n\t"
#define LOAD "vle" STRING(SEW) ".v v8, (%0)\n\t"
#define STORE "vse" STRING(SEW) ".v v8, (%1)\n\t"
#define CLOBBERS "v0", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", \
                 "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "memory"
#define SOURCE ((volatile uint8_t *)(uintptr_t)0x90101000)
#define OUTPUT(slot) ((volatile uint8_t *)(uintptr_t)(0x90104000 + (slot) * 0x800))
#define CONTROL ((volatile uint64_t *)(uintptr_t)0x90120000)
#define CODE ((volatile uint32_t *)(uintptr_t)0x90118000)
#define PATCH ((volatile uint32_t *)(uintptr_t)0x90117000)
#define PMP_SOURCE ((volatile uint32_t *)(uintptr_t)0x90119000)
#define PMP_OUTPUT ((volatile uint32_t *)(uintptr_t)0x90110000)
#define END ((uintptr_t)0x90200000)

extern void trap_handler(void);
extern void benchmark_marker(unsigned long phase, unsigned long start);
volatile unsigned traps, unexpected_traps;
volatile uint64_t trap_causes[3], trap_values[3], trap_starts[3];

static void setvl(unsigned long vl) {
    __asm__ volatile ("vsetvli zero, %0, " VTYPE : : "r"(vl) : "memory");
}

static int peer_exchange(void) {
    volatile uint32_t *start = (volatile uint32_t *)(uintptr_t)0x90100000;
    volatile uint32_t *ready = (volatile uint32_t *)(uintptr_t)0x90100040;
    volatile uint32_t *ack = (volatile uint32_t *)(uintptr_t)0x90100080;
    unsigned attempts = 0;
    while (*start != 0x1badb002) if (++attempts == 100000) return 1;
    int (*code)(void) = (int (*)(void))(uintptr_t)0x901000a0;
    __asm__ volatile ("vsetivli zero, 8, e32, m1, tu, mu\n\t"
        "vle32.v v8, (%0)\n\tvadd.vi v8, v8, 15\n\tvadd.vi v8, v8, 2\n\t"
        "vse32.v v8, (%1)"
        : : "r"((uintptr_t)0x90100004), "r"((uintptr_t)0x90100044) : CLOBBERS);
    // Execute this page only after its vector output store: a translated code
    // page intentionally takes the conservative self-modifying-code path.
    __asm__ volatile ("fence.i" : : : "memory");
    if (code() != 17) return 2;
    *(volatile uint64_t *)(uintptr_t)0x90100068 = UINT64_C(0x1122334455667788);
    __asm__ volatile ("fence rw, rw" : : : "memory");
    *ready = 0xc001c0de;
    attempts = 0;
    while (*ack != 0x600d600d) if (++attempts == 100000) return 3;
    __asm__ volatile ("fence.i" : : : "memory");
    return code() == 29 ? 0 : 4;
}

int main(void) {
    __asm__ volatile ("csrw mtvec, %0" : : "r"(trap_handler) : "memory");
    if (peer_exchange()) return 1;
    unsigned long maximum, vlenb;
    __asm__ volatile ("vsetvli %0, zero, " VTYPE "csrr %1, vlenb"
        : "=&r"(maximum), "=r"(vlenb) : : "memory");
    const unsigned element = SEW / 8;
    const unsigned bytes = maximum * element;
    const unsigned initialized = (bytes > 2 * vlenb ? bytes : 2 * vlenb) + 8;
    for (unsigned i = 0; i < 2 * initialized; ++i) SOURCE[i] = (uint8_t)(i * 13 + 7);
    for (unsigned slot = 0; slot < 10; ++slot)
        for (unsigned i = 0; i < initialized; ++i) OUTPUT(slot)[i] = 0xa5;
    volatile uint8_t *boundary = (volatile uint8_t *)(END - 2 * element);
    for (unsigned i = 0; i < 2 * element; ++i) boundary[i] = 0x3c;
    CODE[0] = 0x01100513; CODE[1] = 0x00008067; // return 17
    PATCH[0] = 0x01d00513; PATCH[1] = 0x00008067; // return 29
    for (unsigned i = 0; i < 8; ++i) PMP_SOURCE[i] = 0x1100 + i;
    __asm__ volatile ("fence.i" : : : "memory");
    int (*generated_code)(void) = (int (*)(void))(uintptr_t)CODE;
    if (generated_code() != 17) return 14;

    benchmark_marker(1, 1);
    setvl(maximum);
    __asm__ volatile (LOAD STORE : : "r"(SOURCE), "r"(OUTPUT(0)) : CLOBBERS);
    benchmark_marker(1, 0);

    benchmark_marker(2, 1);
    *(volatile uint32_t *)OUTPUT(1) = *(volatile uint32_t *)SOURCE;
    benchmark_marker(2, 0);

    benchmark_marker(3, 1);
    setvl(maximum - 1);
    __asm__ volatile (LOAD STORE : : "r"(SOURCE), "r"(OUTPUT(2)) : CLOBBERS);
    benchmark_marker(3, 0);

    benchmark_marker(4, 1);
    setvl(maximum);
    __asm__ volatile ("vid.v v16\n\tvand.vi v16, v16, 1\n\tvmseq.vi v0, v16, 0\n\t"
        "vle" STRING(SEW) ".v v8, (%0), v0.t\n\t"
        "vse" STRING(SEW) ".v v8, (%1), v0.t"
        : : "r"(SOURCE), "r"(OUTPUT(3)) : CLOBBERS);
    benchmark_marker(4, 0);

    benchmark_marker(5, 1);
    setvl(maximum);
    __asm__ volatile (LOAD STORE : : "r"(SOURCE + 1), "r"(OUTPUT(4) + 1) : CLOBBERS);
    benchmark_marker(5, 0);

    benchmark_marker(6, 1);
    setvl(maximum);
    __asm__ volatile ("csrwi vstart, 1\n\t" LOAD "csrwi vstart, 1\n\t" STORE
        : : "r"(SOURCE), "r"(OUTPUT(5)) : CLOBBERS);
    benchmark_marker(6, 0);

    benchmark_marker(7, 1);
    setvl(4);
    __asm__ volatile ("vmv.v.x v8, %2\n\tvid.v v16\n\tvmsltu.vi v0, v16, 2\n\t"
        "vle" STRING(SEW) ".v v8, (%0), v0.t\n\t" STORE
        : : "r"(boundary), "r"(OUTPUT(6)), "r"(90ul) : CLOBBERS);
    benchmark_marker(7, 0);
    if (traps) return 2; // masked-off invalid suffix must not fault

    benchmark_marker(8, 1);
    setvl(4);
    __asm__ volatile ("vmv.v.x v8, %2\n\t" LOAD STORE
        : : "r"(boundary), "r"(OUTPUT(7)), "r"(90ul) : CLOBBERS);
    benchmark_marker(8, 0);
    if (traps != 1) return 3;

    benchmark_marker(9, 1);
    setvl(4);
    __asm__ volatile (LOAD STORE : : "r"(SOURCE), "r"(boundary) : CLOBBERS);
    benchmark_marker(9, 0);
    if (traps != 2 || unexpected_traps) return 4;

    benchmark_marker(10, 1);
    setvl(maximum);
    __asm__ volatile ("vlse" STRING(SEW) ".v v8, (%0), %2\n\t" STORE
        : : "r"(SOURCE), "r"(OUTPUT(8)), "r"((unsigned long)(2 * element)) : CLOBBERS);
    benchmark_marker(10, 0);

    benchmark_marker(11, 1);
    __asm__ volatile ("vsetivli zero, 1, e32, m1, tu, mu\n\t"
        "vl2re8.v v8, (%0)\n\tvs2r.v v8, (%1)"
        : : "r"(SOURCE), "r"(OUTPUT(9)) : CLOBBERS);
    benchmark_marker(11, 0);

    benchmark_marker(12, 1);
    __asm__ volatile ("vsetivli zero, 2, e32, m1, tu, mu\n\t"
        "vle32.v v8, (%0)\n\tvse32.v v8, (%1)\n\tfence.i"
        : : "r"(PATCH), "r"(CODE) : CLOBBERS);
    if (generated_code() != 29) return 15;
    benchmark_marker(12, 0);

    benchmark_marker(13, 1);
    // A locked 16-byte NAPOT region denies words 4..7 even in M mode. PMP
    // updates flush the TLB, exercising a fresh subpage mapping on the load.
    __asm__ volatile ("csrw pmpaddr0, %0\n\tcsrw pmpcfg0, %1\n\t"
        "vsetivli zero, 8, e32, m1, tu, mu\n\tvmv.v.i v8, -1\n\t"
        "vle32.v v8, (%2)\n\tvse32.v v8, (%3)"
        : : "r"(((uintptr_t)0x90119010 >> 2) | 1), "r"(0x98ul),
            "r"(PMP_SOURCE), "r"(PMP_OUTPUT) : CLOBBERS);
    benchmark_marker(13, 0);
    if (traps != 3 || unexpected_traps || trap_causes[2] != 5 ||
        trap_values[2] != 0x90119010 || trap_starts[2] != 4) return 16;
    for (unsigned i = 0; i < 8; ++i)
        if (PMP_OUTPUT[i] != (i < 4 ? 0x1100 + i : UINT32_MAX)) return 17;

    CONTROL[0] = SEW; CONTROL[1] = maximum; CONTROL[2] = bytes;
    CONTROL[3] = initialized; CONTROL[4] = traps;
    for (unsigned i = 0; i < 3; ++i) {
        CONTROL[5 + i * 3] = trap_causes[i];
        CONTROL[6 + i * 3] = trap_values[i];
        CONTROL[7 + i * 3] = trap_starts[i];
        if (i < 2 && (trap_causes[i] != (i ? 7u : 5u) ||
            trap_values[i] != END || trap_starts[i] != 2)) return 5;
    }
    for (unsigned i = 0; i < bytes; ++i) {
        const unsigned lane = i / element;
        const uint8_t pattern = (uint8_t)(i * 13 + 7);
        if (OUTPUT(0)[i] != pattern) return 6;
        if (OUTPUT(2)[i] != (lane < maximum - 1 ? pattern : 0xa5)) return 7;
        if (OUTPUT(3)[i] != (lane % 2 == 0 ? pattern : 0xa5)) return 8;
        if (OUTPUT(4)[i + 1] != (uint8_t)((i + 1) * 13 + 7)) return 9;
        if (OUTPUT(5)[i] != (lane ? pattern : 0xa5)) return 10;
        if (OUTPUT(8)[i] != (uint8_t)((2 * lane * element + i % element) * 13 + 7)) return 11;
    }
    for (unsigned i = 0; i < 2 * vlenb; ++i)
        if (OUTPUT(9)[i] != (uint8_t)(i * 13 + 7)) return 12;
    for (unsigned i = 0; i < 2 * element; ++i)
        if (boundary[i] != (uint8_t)(i * 13 + 7)) return 13;
    return 0;
}
