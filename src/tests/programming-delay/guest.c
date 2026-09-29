#include <stdint.h>
#ifndef DIMENSION
#define DIMENSION 32
#endif
#ifndef LMUL
#define LMUL m1
#endif
#define STRING_(x) #x
#define STRING(x) STRING_(x)
#define CONFIG "vsetvli %0, %1, e32, " STRING(LMUL) ", tu, mu\n\t"
#define CLOBBERS "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "memory"
static float weights[DIMENSION * DIMENSION], input[DIMENSION];
extern void benchmark_marker(unsigned, unsigned);

int main(void) {
    for (unsigned i = 0; i < DIMENSION * DIMENSION; ++i) weights[i] = (int)(i % 11) - 5;
    for (unsigned i = 0; i < DIMENSION; ++i) input[i] = i % 7 + 1;
    benchmark_marker(1, 1);
    for (unsigned i = 0; i < DIMENSION * DIMENSION;) {
        unsigned long vl;
        __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vset v8, zero, %3"
            : "=&r"(vl) : "r"((unsigned long)(DIMENSION * DIMENSION - i)),
              "r"(weights + i), "r"((unsigned long)i) : CLOBBERS);
        i += vl;
    }
    benchmark_marker(1, 0);
    for (unsigned i = 0; i < DIMENSION;) {
        unsigned long vl;
        __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vl v8, zero, %3"
            : "=&r"(vl) : "r"((unsigned long)(DIMENSION - i)), "r"(input + i), "r"((unsigned long)i) : CLOBBERS);
        i += vl;
    }
    unsigned long status;
    __asm__ volatile ("mvm %0, zero, zero" : "=r"(status) : : "memory");
    if (status) return 1;
    float* result = (float*)(uintptr_t)0x90100000;
    for (unsigned i = 0; i < DIMENSION;) {
        unsigned long vl;
        __asm__ volatile (CONFIG "mvm.vs v8, zero, %3\n\tvse32.v v8, (%2)"
            : "=&r"(vl) : "r"((unsigned long)(DIMENSION - i)), "r"(result + i), "r"((unsigned long)i) : CLOBBERS);
        i += vl;
    }
    __asm__ volatile ("fence rw, rw" : : : "memory");
    return 0;
}
