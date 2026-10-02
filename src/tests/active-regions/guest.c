#include <stdint.h>
#ifndef LMUL
#define LMUL m8
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
#define VTYPE "e32, " STR(LMUL) ", tu, mu\n\t"
#define CLOBBERS "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "memory"
static float weights[17 * 19], inputs[19];
static uint32_t captured[256] __attribute__((aligned(64)));
static volatile uint64_t shape;
static unsigned output_offset;

static unsigned long configure(unsigned long slot, unsigned rows, unsigned cols) {
    unsigned long status;
    shape = ((uint64_t)rows << 32) | cols;
    __asm__ volatile (".insn r 0x0b, 7, 9, %0, %1, %2"
        : "=r"(status) : "r"(slot), "r"(shape) : "memory");
    return status;
}
static unsigned long execute(void) {
    unsigned long status;
    __asm__ volatile ("mvm %0, zero, zero" : "=r"(status) : : "memory");
    return status;
}
static void program(unsigned rows, unsigned cols) {
    for (unsigned r = 0; r < rows; ++r) {
        for (unsigned c = 0; c < cols; ++c) weights[r * cols + c] = (int)((r * 19 + c) % 7) - 3;
        for (unsigned c = 0; c < cols;) {
            unsigned long vl;
            __asm__ volatile ("vsetvli %0, %1, " VTYPE "vle32.v v8, (%2)\n\tmvm.vset v8, zero, %3"
                : "=&r"(vl) : "r"((unsigned long)(cols-c)), "r"(weights + r*cols+c),
                  "r"((unsigned long)(r*32+c)) : CLOBBERS);
            c += vl;
        }
    }
}
static int invocation(unsigned rows, unsigned cols, unsigned phase) {
    for (unsigned c = 0; c < cols; ++c) inputs[c] = c % 5 + 1 + phase;
    for (unsigned c = 0; c < cols;) {
        unsigned long vl;
        __asm__ volatile ("vsetvli %0, %1, " VTYPE "vle32.v v8, (%2)\n\tmvm.vl v8, zero, %3"
            : "=&r"(vl) : "r"((unsigned long)(cols-c)), "r"(inputs+c), "r"((unsigned long)c) : CLOBBERS);
        c += vl;
    }
    if (execute()) return 1;
    if (!configure(0, 1, 1)) return 2; // pending result: return an error, never deadlock
    for (unsigned r = 0; r < rows;) {
        unsigned long vl, maximum;
        __asm__ volatile (
            "vsetvli %0, zero, " VTYPE "vmv.v.i v8, 7\n\t"
            "vsetvli %1, %2, " VTYPE "mvm.vs v8, zero, %3\n\t"
            "vsetvli zero, %0, " VTYPE "vse32.v v8, (%4)"
            : "=&r"(maximum), "=&r"(vl)
            : "r"((unsigned long)(rows-r)), "r"((unsigned long)r), "r"(captured) : CLOBBERS);
        for (unsigned i = 0; i < vl; ++i) {
            union { uint32_t bits; float value; } result = {.bits = captured[i]};
            float expected = 0;
            for (unsigned c = 0; c < cols; ++c) expected += weights[(r+i)*cols+c] * inputs[c];
            if (result.value != expected) return 3;
            ((volatile float*)(uintptr_t)0x90100000)[output_offset++] = result.value;
        }
        for (unsigned i = vl; i < maximum; ++i) if (captured[i] != 7) return 4;
        r += vl;
    }
    return 0;
}
int main(void) {
    if (!configure(0,0,19) || !configure(0,17,0) || !configure(0,33,19) ||
        !configure(0,17,33) || !configure(1,17,19) || !configure(1UL<<32,17,19)) return 10;
    if (configure(0,17,19) || !execute()) return 11;
    program(17,19);
    if (invocation(17,19,0) || invocation(17,19,1)) return 12;
    if (configure(0,3,5) || !execute()) return 13;
    program(3,5); if (invocation(3,5,2)) return 14;
    if (configure(0,1,1) || !execute()) return 15;
    program(1,1); if (invocation(1,1,3)) return 16;
    if (configure(0,17,19) || !execute()) return 17;
    program(17,19); if (invocation(17,19,4)) return 18;
    __asm__ volatile ("fence rw, rw" : : : "memory");
    return 0;
}
