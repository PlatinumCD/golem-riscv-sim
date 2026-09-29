#include <stdint.h>

#ifndef LMUL
#define LMUL m1
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
#define CONFIG "vsetvli %0, %1, e32, " STR(LMUL) ", tu, mu\n\t"
#define CLOBBERS "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "memory"
#define ROWS 17
#define COLS 19
static float weights[ROWS * COLS], input[COLS], result[ROWS];
volatile unsigned traps, unexpected_traps;
extern void trap_handler(void);

static unsigned program(unsigned array, unsigned offset, const float *p, unsigned remaining) {
    unsigned long vl;
    if (remaining > 256) remaining = 256; // exercise the full 1024-byte m8 payload
    __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vset v8, %3, %4"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(p), "r"((unsigned long)array),
          "r"((unsigned long)offset) : CLOBBERS);
    return vl;
}
static unsigned load(unsigned array, unsigned offset, const float *p, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vl v8, %3, %4"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(p), "r"((unsigned long)array),
          "r"((unsigned long)offset) : CLOBBERS);
    return vl;
}
static unsigned store(unsigned array, unsigned offset, float *p, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG "mvm.vs v8, %3, %4\n\tvse32.v v8, (%2)"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(p), "r"((unsigned long)array),
          "r"((unsigned long)offset) : CLOBBERS);
    return vl;
}
static unsigned compute(unsigned array) {
    unsigned long status;
    __asm__ volatile ("mvm %0, %1, %1" : "=r"(status) : "r"((unsigned long)array) : "memory");
    return status;
}
static int verify(unsigned array, unsigned scale) {
    if (compute(array)) return 1;
    for (unsigned i = 0; i < ROWS;) i += store(array, i, result + i, ROWS - i);
    for (unsigned r = 0; r < ROWS; ++r) {
        float expected = 0;
        for (unsigned c = 0; c < COLS; ++c) expected += weights[r * COLS + c] * input[c];
        if (result[r] != expected * scale) return 2;
    }
    return 0;
}

// Every deliberately malformed custom instruction is four bytes. The handler
// checks mcause and skips it, allowing errors to be verified without trap loops.
static int errors(void) {
    __asm__ volatile ("csrw mtvec, %0" : : "r"(trap_handler) : "memory");
    unsigned long array = 0, offset = COLS, bad = UINT64_MAX;
    uint32_t sentinel = 0;
    __asm__ volatile ("vsetivli zero, 1, e32, m1, tu, mu\n\t"
        "vmv.v.i v8, -1\n\t"
        "mvm.vl v8, %0, %1\n\t"  // input bounds
        "mvm.vset v8, %0, %2\n\t" // overflow-safe matrix bounds
        "mvm.vs v8, %0, %2\n\t"   // output bounds
        "vse32.v v8, (%3)\n\t"   // failed output leaves its destination intact
        "mvm.vl v8, %2, zero\n\t" // array ID must not truncate
        "vsetivli zero, 1, e16, m1, tu, mu\n\t"
        "mvm.vl v8, %0, zero\n\t" // wrong SEW
        "vsetivli zero, 1, e32, m1, tu, mu\n\t"
        "csrwi vstart, 1\n\t"
        "mvm.vl v8, %0, zero\n\t" // nonzero vstart
        "vsetivli zero, 1, e32, m2, tu, mu\n\t"
        "mvm.vl v9, %0, zero\n\t" // misaligned register group
        "vsetivli zero, 1, e32, m1, tu, mu\n\t"
        : : "r"(array), "r"(offset), "r"(bad), "r"(&sentinel) : CLOBBERS);
    if (traps != 7 || unexpected_traps || sentinel != UINT32_MAX) return 1;
    // VS=Off must reject the instruction without disabling the scalar handler.
    unsigned long vs = 0x600;
    __asm__ volatile ("csrc mstatus, %0\n\tmvm.vl v8, zero, zero\n\tcsrs mstatus, %0"
        : : "r"(vs) : CLOBBERS);
    if (traps != 8 || unexpected_traps) return 1;
    // Retired memory/direct-array instructions must reject before touching
    // this address outside SPM or submitting any analog command.
    __asm__ volatile ("mvm.set zero, %0, zero\n\tmvm.l zero, %0, zero\n\t"
        "mvm.s zero, %0, zero\n\tmvm.mv zero, %0, zero"
        : : "r"((unsigned long)0x80000000) : "memory");
    return traps != 12 || unexpected_traps;
}

int main(void) {
    for (unsigned i = 0; i < ROWS * COLS; ++i) weights[i] = (int)(i % 7) - 3;
    for (unsigned i = 0; i < COLS; ++i) input[i] = i % 5 + 1;
    if (!compute(0)) return 10; // uninitialized array
    program(0, 7, weights + 7, 1);
    load(0, 3, input + 3, 1);
    if (!compute(0)) return 11; // incomplete first matrix/input
    // An empty transfer at the capacity boundary must not initialize/invalidate
    // state, require computed output, or charge programming/link service.
    __asm__ volatile ("vsetivli zero, 0, e32, m1, tu, mu\n\t"
        "mvm.vset v8, zero, %0\n\tmvm.vl v8, zero, %1\n\tmvm.vs v8, zero, %2"
        : : "r"((unsigned long)(ROWS * COLS)), "r"((unsigned long)COLS),
          "r"((unsigned long)ROWS) : CLOBBERS);
    for (unsigned a = 0; a < 2; ++a) {
        for (unsigned i = 0; i < ROWS * COLS;) i += program(a, i, weights + i, ROWS * COLS - i);
        for (unsigned i = 0; i < COLS;) i += load(a, i, input + i, COLS - i);
        if (verify(a, 1)) return 12 + a;
    }
    // Modifying one array must preserve all other elements and the other array.
    const float old_weight = weights[COLS + 2], old_input = input[3];
    weights[COLS + 2] = 7; input[3] = 9;
    program(0, COLS + 2, weights + COLS + 2, 1);
    load(0, 3, input + 3, 1);
    if (verify(0, 1)) return 14;
    weights[COLS + 2] = old_weight; input[3] = old_input;
    if (verify(1, 1)) return 15;
    if (errors()) return 16;
    // Bad transfers did not change either array's valid state or values.
    weights[COLS + 2] = 7; input[3] = 9;
    if (verify(0, 1)) return 19;
    weights[COLS + 2] = old_weight; input[3] = old_input;
    if (verify(1, 1)) return 17;
    // verify() consumed every row of the previous pipelined result. Produce
    // a fresh result before testing that empty writes preserve output state.
    if (compute(1)) return 20;
    __asm__ volatile ("vsetivli zero, 0, e32, m1, tu, mu\n\tli t0, 1\n\t"
        "mvm.vset v8, t0, zero\n\tmvm.vl v8, t0, zero" : : : "t0", CLOBBERS);
    // Store without recomputing checks that empty writes preserve valid output.
    store(1, 0, result, 1);
    // A partial output must preserve destination tail elements even for ta.
    uint32_t tail[32];
    unsigned long maximum;
    __asm__ volatile ("vsetvli %0, zero, e32, m1, ta, ma\n\t"
        "vmv.v.i v8, -1\n\tvsetivli zero, 1, e32, m1, ta, ma\n\t"
        "li t0, 1\n\tmvm.vs v8, t0, zero\n\t"
        "vsetvli zero, %0, e32, m1, tu, mu\n\tvse32.v v8, (%1)"
        : "=&r"(maximum) : "r"(tail) : "t0", CLOBBERS);
    for (unsigned i = 1; i < maximum; ++i) if (tail[i] != UINT32_MAX) return 18;
    if (traps != 12 || unexpected_traps) return 21;
    // Host independently checks actual bytes written to the shared SPM.
    volatile float *mailbox = (volatile float *)0x90100000;
    for (unsigned i = 0; i < ROWS; ++i) mailbox[i] = result[i];
    return 0;
}
