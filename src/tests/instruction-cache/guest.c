#include <stdint.h>

extern void benchmark_marker(unsigned long phase, unsigned long start);
extern unsigned hot_loop(unsigned iterations);
extern unsigned conflict_loop(unsigned iterations);
extern unsigned boundary_instruction(void);
extern unsigned small_boundary_instruction(void);
extern unsigned modified_code(void);

int main(void) {
    unsigned results[6];
    benchmark_marker(1, 1);
    results[0] = hot_loop(128);
    benchmark_marker(1, 0);
    benchmark_marker(2, 1);
    results[1] = conflict_loop(32);
    benchmark_marker(2, 0);
    benchmark_marker(3, 1);
    results[2] = boundary_instruction();
    results[5] = small_boundary_instruction();
    benchmark_marker(3, 0);
    benchmark_marker(4, 1);
    results[3] = modified_code();
    /* addi a0, zero, 29 replaces addi a0, zero, 17. Both are 32-bit
       instructions; fence.i must invalidate SST timing and QEMU translation. */
    *(volatile uint32_t *)(uintptr_t)modified_code = UINT32_C(0x01d00513);
    __asm__ volatile ("fence.i" : : : "memory");
    results[4] = modified_code();
    benchmark_marker(4, 0);

    volatile uint32_t *mailbox = (volatile uint32_t *)(uintptr_t)0x90100000;
    const unsigned expected[6] = {384, 192, 37, 17, 29, 41};
    for (unsigned i = 0; i < 6; ++i) {
        mailbox[i] = results[i];
        if (results[i] != expected[i]) return 1 + i;
    }
    return 0;
}
