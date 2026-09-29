/* Real RV64 guests consume network writes through their shared SPM banks.
 * Explicit remote requests originate in the test initiator, outside the CPU.
 * Endpoint components never poll these test-only coordination flags. */
#include "layout.h"
#ifndef TILE_ID
#define TILE_ID 0
#endif
#ifndef BANKS
#define BANKS 4
#endif
#ifndef BANK_WIDTH
#define BANK_WIDTH 4
#endif
#ifndef ROUTER_BANK0
#define ROUTER_BANK0 2
#endif
#ifndef ROUTER_BANK1
#define ROUTER_BANK1 3
#endif
#define VECTOR_GROUP "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15"
#define RESULT ((volatile struct SharedBankTestResult *)(TEST_SPM_BASE + TEST_RESULT_OFFSET))
#define FLAG(offset) (*(volatile uint32_t *)(TEST_SPM_BASE + (offset) + ROUTER_BANK0 * BANK_WIDTH))
_Static_assert(BANK_WIDTH >= 4 && BANK_WIDTH % 4 == 0, "FP32 bank width");
_Static_assert(TEST_WORDS % (BANK_WIDTH / 4) == 0, "Complete bank stripes");
static inline void fence(void) { __asm__ volatile ("fence rw, rw" : : : "memory"); }
static uint32_t *word_address(unsigned base, unsigned word) {
    return (uint32_t *)(TEST_SPM_BASE + test_word_offset(base, word, BANKS, BANK_WIDTH,
                                                       ROUTER_BANK0, ROUTER_BANK1));
}
static int fail(unsigned code, unsigned detail0, unsigned detail1) {
    RESULT->error_code = code; RESULT->detail0 = detail0; RESULT->detail1 = detail1;
    fence(); RESULT->status = UINT32_MAX; fence(); return code;
}
static unsigned gather(const uint32_t *source, uint32_t *destination, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (
        "vsetvli %0, %1, e32, m8, tu, mu\n\t"
        "vle32.v v8, (%2)\n\t"
        "vse32.v v8, (%3)"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(source), "r"(destination)
        : VECTOR_GROUP, "memory");
    return (unsigned)vl;
}
static unsigned fill_source(uint32_t *destination, unsigned first, unsigned remaining) {
    unsigned long vl;
    const unsigned long first_value = TILE_ID * 32u + first;
    __asm__ volatile (
        "vsetvli %0, %1, e32, m8, tu, mu\n\t"
        "vid.v v8\n\t"
        "vadd.vx v8, v8, %3\n\t"
        "vand.vx v8, v8, %4\n\t"
        "vfcvt.f.xu.v v8, v8\n\t"
        "vse32.v v8, (%2)"
        : "=&r"(vl)
        : "r"((unsigned long)remaining), "r"(destination), "r"(first_value),
          "r"((unsigned long)127)
        : VECTOR_GROUP, "memory");
    return (unsigned)vl;
}

static unsigned program_array(const uint32_t *source, unsigned offset, unsigned remaining) {
    unsigned long vl;
    if (remaining > 256) remaining = 256; /* Shared analog payload limit1024B. */
    __asm__ volatile (
        "vsetvli %0, %1, e32, m8, tu, mu\n\t"
        "vle32.v v8, (%2)\n\t"
        "mvm.vset v8, zero, %3"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(source),
          "r"((unsigned long)offset) : VECTOR_GROUP, "memory");
    return (unsigned)vl;
}

static unsigned load_array(const uint32_t *source, unsigned offset, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (
        "vsetvli %0, %1, e32, m8, tu, mu\n\t"
        "vle32.v v8, (%2)\n\t"
        "mvm.vl v8, zero, %3"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(source),
          "r"((unsigned long)offset) : VECTOR_GROUP, "memory");
    return (unsigned)vl;
}

static unsigned store_array(uint32_t *destination, unsigned offset, unsigned remaining) {
    unsigned long vl;
    __asm__ volatile (
        "vsetvli %0, %1, e32, m8, tu, mu\n\t"
        "mvm.vs v8, zero, %3\n\t"
        "vse32.v v8, (%2)"
        : "=&r"(vl) : "r"((unsigned long)remaining), "r"(destination),
          "r"((unsigned long)offset) : VECTOR_GROUP, "memory");
    return (unsigned)vl;
}

int main(void) {
    RESULT->magic = TEST_MAGIC; RESULT->tile = TILE_ID; RESULT->phase = 1;
    RESULT->bank_width = BANK_WIDTH;
    RESULT->router_bank0 = ROUTER_BANK0; RESULT->router_bank1 = ROUTER_BANK1;
    volatile uint8_t *probe = (volatile uint8_t *)(TEST_SPM_BASE + TEST_PROBE_OFFSET);
    for (unsigned i = 0; i < 2 * BANKS * BANK_WIDTH; ++i) probe[i] = 0x5a;
    for (unsigned i = 0; i < TEST_WORDS;)
        i += fill_source(word_address(TEST_SOURCE_OFFSET, i), i, BANK_WIDTH / 4);
    RESULT->source_words = TEST_WORDS;
    fence(); FLAG(TEST_READY_OFFSET) = TEST_READY_VALUE | TILE_ID; fence();
    RESULT->phase = 2;
    unsigned polls = 0;
    while (FLAG(TEST_NOTIFY_OFFSET) != (TEST_NOTIFY_VALUE | TILE_ID)) {
        /* Independent RVV reads create real shared-port contention while
         * network requests arrive. This region stays initialized/stable. */
        unsigned long vl;
        __asm__ volatile ("vsetvli %0, %1, e32, m8, tu, mu\n\t"
                          "vle32.v v8, (%2)"
            : "=&r"(vl) : "r"((unsigned long)(2 * BANKS * BANK_WIDTH / 4)), "r"(probe)
            : VECTOR_GROUP, "memory");
        if (++polls == 1000000u) return fail(10, polls, 0);
    }
    fence(); RESULT->phase = 3;
    uint32_t *dense = (uint32_t *)(TEST_SPM_BASE + TEST_DENSE_OFFSET);
    const unsigned owner = (TILE_ID + 3u) % 4u;
    for (unsigned i = 0; i < TEST_WORDS;)
        i += gather(word_address(TEST_INPUT_OFFSET, i), dense + i, BANK_WIDTH / 4);
    fence();
    for (unsigned i = 0; i < TEST_WORDS; ++i) {
        const unsigned actual = ((volatile uint32_t *)dense)[i];
        if (actual != test_float_bits(owner, i)) return fail(11, i, actual);
        RESULT->payload_checksum += actual;
    }
    RESULT->verified_bytes = TEST_WORDS * 4;
    for (unsigned i = 0; i < 2 * BANKS * BANK_WIDTH; ++i)
        if (probe[i] != 0x5a) return fail(12, i, probe[i]);
    RESULT->private_guard_bytes = 2 * BANKS * BANK_WIDTH;
    RESULT->phase = 4;
    volatile uint32_t *weights = (volatile uint32_t *)(TEST_SPM_BASE + TEST_WEIGHTS_OFFSET);
    for (unsigned i = 0; i < 32 * 32; ++i) weights[i] = i % 33 == 0 ? UINT32_C(0x3f800000) : 0;
    fence();
    for (unsigned i = 0; i < 32 * 32;) i += program_array((const uint32_t *)weights + i, i, 32 * 32 - i);
    for (unsigned i = 0; i < 32;) i += load_array(dense + i, i, 32 - i);
    unsigned long status;
    __asm__ volatile ("mvm %0, zero, zero" : "=r"(status) : : "memory");
    if (status) return fail(13, (unsigned)status, 0);
    uint32_t *output = (uint32_t *)(TEST_SPM_BASE + TEST_OUTPUT_OFFSET);
    for (unsigned i = 0; i < 32;) i += store_array(output + i, i, 32 - i);
    fence();
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned actual = ((volatile uint32_t *)output)[i];
        if (actual != test_float_bits(owner, i)) return fail(14, i, actual);
        RESULT->mvm_checksum += actual; RESULT->mvm_words++;
    }
    RESULT->phase = 5; fence(); RESULT->status = 1; fence();
    FLAG(TEST_DONE_OFFSET) = TEST_DONE_VALUE | TILE_ID; fence();
    return 0;
}
