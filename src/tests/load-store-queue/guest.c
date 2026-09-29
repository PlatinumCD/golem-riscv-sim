#include <stdint.h>
#ifndef VLEN_BITS
#define VLEN_BITS 256
#endif
#ifndef MODE
#define MODE 0
#endif
#define LANES (VLEN_BITS / 32)
#define SOURCE ((uint32_t *)(uintptr_t)0x90101000)
#define OUTPUT(n) ((uint32_t *)(uintptr_t)(0x90110000 + (n) * 0x1000))
#define ALIAS ((uint32_t *)(uintptr_t)0x90130000)
#define CONTROL ((volatile uint64_t *)(uintptr_t)0x90140000)
extern void benchmark_marker_unfenced(unsigned long, unsigned long);
extern void trap_handler(void);
extern void loads24(void *);
extern void stores24(void *);
extern void load_pair(void *);
extern void raw_kernel(void *, void *);
extern void waw_kernel(void *, void *);
extern void snapshot_kernel(void *, void *);
extern void memory_raw_kernel(void *, void *);
extern void partial_raw_kernel(void *, void *);
extern void memory_waw_kernel(void *);
extern void memory_war_kernel(void *, void *);
extern void alu_snapshot_kernel(void *);
extern void drain_kernel(void *);
extern void fetch_fault_kernel(void *, void *);
volatile uint64_t trap_record[4];
static float weights[32 * 32], input[32];

static void fence(void) { __asm__ volatile ("fence rw, rw" : : : "memory"); }
static void mark(unsigned phase, unsigned start) { benchmark_marker_unfenced(phase, start); }
static void initialize_alias(void) {
    for (unsigned i = 0; i < 2 * LANES; ++i) ALIAS[i] = 0x9000 + i;
}

static int peer_exchange(void) {
    volatile uint32_t *start = (volatile uint32_t *)(uintptr_t)0x90100000;
    volatile uint32_t *ready = (volatile uint32_t *)(uintptr_t)0x90100040;
    volatile uint32_t *ack = (volatile uint32_t *)(uintptr_t)0x90100080;
    unsigned attempts = 0;
    while (*start != 0x1badb002) if (++attempts == 100000) return 1;
    __asm__ volatile ("vsetivli zero, 8, e32, m1, tu, mu\n\t"
        "vle32.v v8, (%0)\n\tvadd.vi v8, v8, 15\n\tvadd.vi v8, v8, 2\n\t"
        "vse32.v v8, (%1)" : : "r"((uintptr_t)0x90100004), "r"((uintptr_t)0x90100044) : "v8", "memory");
    int (*code)(void) = (int (*)(void))(uintptr_t)0x901000a0;
    __asm__ volatile ("fence.i" : : : "memory");
    if (code() != 17) return 2;
    *(volatile uint64_t *)(uintptr_t)0x90100068 = UINT64_C(0x1122334455667788);
    fence(); *ready = 0xc001c0de;
    attempts = 0;
    while (*ack != 0x600d600d) if (++attempts == 100000) return 3;
    __asm__ volatile ("fence.i" : : : "memory");
    return code() == 29 ? 0 : 4;
}

static int analog_hazards(void) {
    for (unsigned row = 0; row < 32; ++row) {
        input[row] = (float)((int)(row % 7) - 3);
        for (unsigned col = 0; col < 32; ++col) weights[row * 32 + col] = row == col ? 1.0f : 0.0f;
    }
    mark(30, 1);
    for (unsigned offset = 0; offset < 32 * 32; offset += LANES)
        __asm__ volatile ("vle32.v v8, (%0)\n\tmvm.vset v8, zero, %1"
            : : "r"(weights + offset), "r"((unsigned long)offset) : "v8", "memory");
    for (unsigned offset = 0; offset < 32; offset += LANES)
        __asm__ volatile ("vle32.v v8, (%0)\n\tmvm.vl v8, zero, %1"
            : : "r"(input + offset), "r"((unsigned long)offset) : "v8", "memory");
    unsigned long status;
    __asm__ volatile ("mvm %0, zero, zero" : "=r"(status) : : "memory");
    if (status) return 5;
    // mvm.vs overwrites v8 while the preceding store may still need its old
    // payload; custom instructions must also respect a pending load to v8.
    for (unsigned offset = 0; offset < 32; offset += LANES)
        __asm__ volatile ("vle32.v v8, (%0)\n\tmvm.vs v8, zero, %2\n\tvse32.v v8, (%1)"
            : : "r"(weights + offset), "r"(OUTPUT(30) + offset), "r"((unsigned long)offset) : "v8", "memory");
    mark(30, 0);
    return 0;
}

int main(void) {
    __asm__ volatile ("csrw mtvec, %0" : : "r"(trap_handler) : "memory");
    if (peer_exchange()) return 1;
    for (unsigned i = 0; i < 24 * LANES; ++i) SOURCE[i] = 0x1000 + i;
    __asm__ volatile ("vsetivli zero, %0, e32, m1, tu, mu" : : "i"(LANES) : "memory");
#if MODE == 5
    return analog_hazards();
#elif MODE == 7
    load_pair(SOURCE);
    __asm__ volatile ("vmv.v.i v8, -1" : : : "v8", "memory");
    mark(21, 1);
    fetch_fault_kernel(SOURCE, OUTPUT(20));
    mark(21, 0);
    for (unsigned i = 0; i < 4; ++i) CONTROL[i] = trap_record[i];
    return trap_record[0] != 1 || trap_record[1] != 0x90200000 ||
           trap_record[2] != 0 || trap_record[3] != 1;
#elif MODE != 0
    loads24(SOURCE); fence();
    mark(20, 1);
    drain_kernel(OUTPUT(20));
    mark(20, 0);
    for (unsigned i = 0; i < 4; ++i) CONTROL[i] = trap_record[i];
#if MODE == 4
    if (trap_record[0] != 5 || trap_record[1] != 0x90200000 || trap_record[2] != 0 || trap_record[3] != 1) return 2;
#endif
    return 0;
#else
    // Warm the exact straight-line instruction blocks before timing queue pressure.
    loads24(SOURCE); fence(); stores24(OUTPUT(0)); fence();
    mark(1, 1); loads24(SOURCE); mark(1, 0);
    stores24(OUTPUT(1)); fence();
    mark(2, 1); stores24(OUTPUT(2)); mark(2, 0);
    mark(3, 1); raw_kernel(SOURCE, OUTPUT(3)); mark(3, 0);
    mark(4, 1); waw_kernel(SOURCE, OUTPUT(4)); mark(4, 0);
    load_pair(SOURCE);
    mark(5, 1); snapshot_kernel(SOURCE, OUTPUT(5)); mark(5, 0);
    initialize_alias(); load_pair(SOURCE);
    mark(6, 1); memory_raw_kernel(ALIAS, OUTPUT(6)); mark(6, 0);
    initialize_alias(); load_pair(SOURCE);
    mark(7, 1); partial_raw_kernel(ALIAS, OUTPUT(7)); mark(7, 0);
    load_pair(SOURCE);
    mark(8, 1); memory_waw_kernel(OUTPUT(8)); mark(8, 0);
    initialize_alias(); load_pair(SOURCE);
    mark(9, 1); memory_war_kernel(ALIAS, OUTPUT(9)); mark(9, 0);
    load_pair(SOURCE);
    mark(10, 1); alu_snapshot_kernel(OUTPUT(10)); mark(10, 0);
    return 0;
#endif
}
