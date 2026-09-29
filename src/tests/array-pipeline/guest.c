#include <stdint.h>

#ifndef ARRAY_DIM
#define ARRAY_DIM 32
#endif
#ifndef PIPELINE
#define PIPELINE 0
#endif
#ifndef EXIT_PENDING
#define EXIT_PENDING 0
#endif
#ifndef DRAIN_KIND
#define DRAIN_KIND 0
#endif
#define JOBS 5
#define CONFIG "vsetvli %0, %1, e32, m1, tu, mu\n\t"

extern void benchmark_marker(unsigned long phase, unsigned long start);
extern void benchmark_marker_unfenced(unsigned long phase, unsigned long start);
extern void trap_handler(void);
volatile unsigned traps, unexpected_traps;
static float weights[ARRAY_DIM * ARRAY_DIM], input[ARRAY_DIM];
static volatile float *const outputs = (volatile float *)(uintptr_t)0x90100000;
static volatile uint32_t *const tails = (volatile uint32_t *)(uintptr_t)0x90110000;
static volatile uint32_t *const control = (volatile uint32_t *)(uintptr_t)0x90120000;

static unsigned program(unsigned offset, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vset v8, zero, %3"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(weights + offset),
          "r"((unsigned long)offset) : "v8", "memory");
    return (unsigned)vl;
}

static unsigned load(unsigned offset, unsigned remaining) {
    unsigned long vl;
    if (remaining > 5) remaining = 5;
    __asm__ volatile (CONFIG "vle32.v v8, (%2)\n\tmvm.vl v8, zero, %3"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(input + offset),
          "r"((unsigned long)offset) : "v8", "memory");
    return (unsigned)vl;
}

static unsigned store(unsigned offset, volatile float *destination, unsigned remaining) {
    unsigned long vl;
    if (remaining > 7) remaining = 7;
    __asm__ volatile (CONFIG "mvm.vs v8, zero, %3\n\tvse32.v v8, (%2)"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(destination),
          "r"((unsigned long)offset) : "v8", "memory");
    return (unsigned)vl;
}

static unsigned compute(void) {
    unsigned long status;
    __asm__ volatile ("mvm %0, zero, zero" : "=r"(status) : : "memory");
    return (unsigned)status;
}

__attribute__((noinline)) static void prepare_and_load(unsigned job) {
    // These ordinary CPU stores and vector loads occur while the previous
    // array computation is active in pipeline mode. Every input changes.
    for (unsigned c = 0; c < ARRAY_DIM; ++c)
        input[c] = (float)((int)((c * 3 + job * 5) % 13) - 6);
    for (unsigned offset = 0; offset < ARRAY_DIM;)
        offset += load(offset, ARRAY_DIM - offset);
}

__attribute__((noinline)) static void drain(unsigned job) {
    volatile float *destination = outputs + job * ARRAY_DIM;
    unsigned long maximum;
    // Read the final three outputs first. In ta mode the custom instruction
    // must leave the remaining vector elements untouched. Reading these three
    // outputs again below must not count them twice toward FIFO retirement.
    __asm__ volatile (
        "vsetvli %0, zero, e32, m1, ta, ma\n\t"
        "vmv.v.i v8, -1\n\t"
        "vsetivli zero, 3, e32, m1, ta, ma\n\t"
        "mvm.vs v8, zero, %1\n\t"
        "vsetvli zero, %0, e32, m1, tu, mu\n\t"
        "vse32.v v8, (%2)"
        : "=&r"(maximum) : "r"((unsigned long)(ARRAY_DIM - 3)),
          "r"(tails + job * 16) : "v8", "memory");
    control[0] = (uint32_t)maximum;
    store(ARRAY_DIM - 3, destination + ARRAY_DIM - 3, 3);
    for (unsigned offset = 0; offset < ARRAY_DIM - 3;)
        offset += store(offset, destination + offset, ARRAY_DIM - 3 - offset);
}

int main(void) {
    __asm__ volatile ("csrw mtvec, %0" : : "r"(trap_handler) : "memory");
    for (unsigned r = 0; r < ARRAY_DIM; ++r)
        for (unsigned c = 0; c < ARRAY_DIM; ++c)
            weights[r * ARRAY_DIM + c] = (float)((int)((r * 3 + c * 5) % 11) - 5);
    // Invalid execute status must be synchronous; neither failure may create
    // a pipeline job or change the later initialized results.
    if (!compute()) return 1;
    program(0, 1);
    input[0] = 1;
    load(0, 1);
    if (!compute()) return 2;
    for (unsigned offset = 0; offset < ARRAY_DIM * ARRAY_DIM;)
        offset += program(offset, ARRAY_DIM * ARRAY_DIM - offset);

#if EXIT_PENDING
    prepare_and_load(0);
    if (compute()) return 3;
#if DRAIN_KIND == 1
    __asm__ volatile ("fence.i" : : : "memory");
#elif DRAIN_KIND == 2
    // No preceding guest fence: the marker itself must drain async work.
    benchmark_marker_unfenced(2, 1);
    benchmark_marker_unfenced(2, 0);
#endif
    // CPU exit must wait for async Complete; an unread result is legal.
    return 0;
#endif
    benchmark_marker(1, 1);
#if PIPELINE
    prepare_and_load(0);
    if (compute()) return 3;
    weights[0] = 99;
    program(0, 1); // pending compute/result must reject weight mutation
    weights[0] = -5;
    if (traps != 1 || unexpected_traps) return 8;
    __asm__ volatile ("vsetivli zero, 0, e32, m1, tu, mu\n\t"
        "mvm.vset v8, zero, zero\n\tmvm.vs v8, zero, zero" : : : "v8", "memory");
    __asm__ volatile ("fence rw, rw" : : : "memory");
    for (unsigned job = 1; job < JOBS; ++job) {
        prepare_and_load(job);
        if (compute()) return 4;
        drain(job - 1);
    }
    drain(JOBS - 1);
#else
    for (unsigned job = 0; job < JOBS; ++job) {
        prepare_and_load(job);
        if (compute()) return 3;
        __asm__ volatile ("fence rw, rw" : : : "memory");
        drain(job);
    }
#endif
    benchmark_marker(1, 0);
    control[1] = traps;

    if (control[0] != 8 && control[0] != 16) return 5;
    for (unsigned job = 0; job < JOBS; ++job) {
        for (unsigned r = 0; r < ARRAY_DIM; ++r) {
            int expected = 0;
            for (unsigned c = 0; c < ARRAY_DIM; ++c)
                expected += ((int)((r * 3 + c * 5) % 11) - 5) *
                            ((int)((c * 3 + job * 5) % 13) - 6);
            if (outputs[job * ARRAY_DIM + r] != (float)expected) return 6;
        }
        for (unsigned i = 3; i < control[0]; ++i)
            if (tails[job * 16 + i] != UINT32_MAX) return 7;
    }
    return 0;
}
