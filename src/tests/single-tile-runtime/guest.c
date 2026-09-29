#include <stdint.h>

#ifndef ARRAY_DIM
#define ARRAY_DIM 32
#endif
#ifndef REPEATS
#define REPEATS 1000
#endif
#ifndef ARRAY_PIPELINE_ENABLED
#define ARRAY_PIPELINE_ENABLED 0
#endif
#if ARRAY_DIM != 32 && ARRAY_DIM != 64
#error "ARRAY_DIM must be 32 or 64"
#endif
#if REPEATS < 1 || REPEATS > 1000
#error "REPEATS must be between 1 and 1000, matching the benchmark harness"
#endif

#define MATRIX_DIM ARRAY_DIM
#define MATRIX_ELEMENTS (MATRIX_DIM * MATRIX_DIM)
#define CONFIG "vsetvli %0, %1, e32, m1, tu, mu\n\t"

/* The harness supplies markers; their transport is not part of this guest. */
extern void benchmark_marker(unsigned long phase, unsigned long start);

static float weights[MATRIX_ELEMENTS];
static float input[MATRIX_DIM];
/* At 1000 repeats, all 1001 results occupy 128128 bytes for dimension 32 or
   256256 bytes for dimension 64. The largest ends at 0x9013e900, below the
   stack near 0x901ff000 in the 2 MiB SPM. */
static volatile float *const mailbox = (volatile float *)(uintptr_t)0x90100000;

static unsigned program_chunk(unsigned offset, const float *source,
                              unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG
        "vle32.v v8, (%2)\n\t"
        "mvm.vset v8, %3, %4"
        : "=&r"(vl)
        : "r"((unsigned long)remaining), "r"(source),
          "r"((unsigned long)0), "r"((unsigned long)offset)
        : "v8", "memory");
    return (unsigned)vl;
}

static unsigned load_chunk(unsigned offset, const float *source,
                           unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG
        "vle32.v v8, (%2)\n\t"
        "mvm.vl v8, %3, %4"
        : "=&r"(vl)
        : "r"((unsigned long)remaining), "r"(source),
          "r"((unsigned long)0), "r"((unsigned long)offset)
        : "v8", "memory");
    return (unsigned)vl;
}

static unsigned store_chunk(unsigned offset, volatile float *destination,
                            unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (CONFIG
        "mvm.vs v8, %3, %4\n\t"
        "vse32.v v8, (%2)"
        : "=&r"(vl)
        : "r"((unsigned long)remaining), "r"(destination),
          "r"((unsigned long)0), "r"((unsigned long)offset)
        : "v8", "memory");
    return (unsigned)vl;
}

static unsigned compute(void) {
    unsigned long status;
    __asm__ volatile ("mvm %0, %1, %1"
        : "=r"(status) : "r"((unsigned long)0) : "memory");
    return (unsigned)status;
}

/* The whole matrix fits in physical array zero and is programmed only once. */
__attribute__((noinline)) static void program_matrix(void) {
    for (unsigned offset = 0; offset < MATRIX_ELEMENTS;) {
        offset += program_chunk(offset, weights + offset, MATRIX_ELEMENTS - offset);
    }
}

static void load_input(void) {
    for (unsigned offset = 0; offset < MATRIX_DIM;) {
        offset += load_chunk(offset, input + offset, MATRIX_DIM - offset);
    }
}

static void store_output(volatile float *output) {
    for (unsigned offset = 0; offset < MATRIX_DIM;) {
        offset += store_chunk(offset, output + offset, MATRIX_DIM - offset);
    }
}

/* Each MVM loads MATRIX_DIM inputs, computes once with resident weights, and
   writes MATRIX_DIM outputs. All vector transfers and CPU loop/control work
   are measured. */
__attribute__((noinline)) static unsigned run_mvm(volatile float *output) {
    load_input();
    if (compute()) return 1;
    store_output(output);
    return 0;
}

#if ARRAY_PIPELINE_ENABLED
/* Keep at most two jobs resident: start the next computation before draining
   the preceding result. Input preparation and output handling share the CPU;
   either can overlap the array's single compute engine. */
__attribute__((noinline)) static unsigned run_pipeline(volatile float *output) {
    load_input();
    if (compute()) return 1;
    for (unsigned repeat = 1; repeat < REPEATS; ++repeat) {
        load_input();
        if (compute()) return 1;
        store_output(output + (repeat - 1) * MATRIX_DIM);
    }
    store_output(output + (REPEATS - 1) * MATRIX_DIM);
    return 0;
}
#endif

int main(void) {
    /* Matrix/input initialization is outside all timed regions. */
    for (unsigned r = 0; r < MATRIX_DIM; ++r) {
        for (unsigned c = 0; c < MATRIX_DIM; ++c) {
            weights[r * MATRIX_DIM + c] =
                (float)((int)((r * 3 + c * 5) % 11) - 5);
        }
    }
    for (unsigned c = 0; c < MATRIX_DIM; ++c) {
        input[c] = (float)((int)(c % 5) - 2);
    }

    benchmark_marker(0, 1);
    benchmark_marker(0, 0);
    benchmark_marker(1, 1);
    program_matrix();
    benchmark_marker(1, 0);
    benchmark_marker(2, 1);
    unsigned failed = run_mvm(mailbox);
    benchmark_marker(2, 0);
    benchmark_marker(3, 1);
#if ARRAY_PIPELINE_ENABLED
    failed |= run_pipeline(mailbox + MATRIX_DIM);
#else
    for (unsigned repeat = 0; repeat < REPEATS; ++repeat) {
        failed |= run_mvm(mailbox + (repeat + 1) * MATRIX_DIM);
    }
#endif
    benchmark_marker(3, 0);

    if (failed) return 1;
    /* Integer-valued floats make every product, partial sum, and final sum
       exact. Check all outputs after timing with an unpacked scalar oracle. */
    for (unsigned r = 0; r < MATRIX_DIM; ++r) {
        int expected = 0;
        for (unsigned c = 0; c < MATRIX_DIM; ++c) {
            const int weight = (int)((r * 3 + c * 5) % 11) - 5;
            const int value = (int)(c % 5) - 2;
            expected += weight * value;
        }
        for (unsigned run = 0; run <= REPEATS; ++run) {
            if (mailbox[run * MATRIX_DIM + r] != (float)expected) return 2;
        }
    }
    return 0;
}
