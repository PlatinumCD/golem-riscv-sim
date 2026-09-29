#ifndef MORDRED_SHARED_BANK_TEST_LAYOUT_H
#define MORDRED_SHARED_BANK_TEST_LAYOUT_H
#include <stdint.h>
#define TEST_SPM_BASE UINT64_C(0x90000000)
#define TEST_READY_OFFSET 0x100000u
#define TEST_NOTIFY_OFFSET 0x100100u
#define TEST_DONE_OFFSET 0x100200u
#define TEST_RESULT_OFFSET 0x101000u
#define TEST_SOURCE_OFFSET 0x110000u
#define TEST_INPUT_OFFSET 0x120000u
#define TEST_DENSE_OFFSET 0x130000u
#define TEST_OUTPUT_OFFSET 0x140000u
#define TEST_WEIGHTS_OFFSET 0x150000u
#define TEST_PROBE_OFFSET 0x160000u
#define TEST_ARENA_BYTES 4096u
#define TEST_WORDS 64u
#define TEST_READY_VALUE 0x52445900u
#define TEST_NOTIFY_VALUE 0x4e4f5400u
#define TEST_DONE_VALUE 0x444f4e00u
#define TEST_MAGIC 0x53424e4bu
/* Test-only sparse layout. Endpoints never poll these guest/fixture flags. */
static inline uint32_t test_word_offset(uint32_t base, unsigned word,
                                       unsigned banks, unsigned width,
                                       unsigned first_bank, unsigned second_bank) {
    const unsigned words_per_bank = width / 4;
    const unsigned group = word / words_per_bank;
    const unsigned bank = group % 2 ? second_bank : first_bank;
    return base + (group / 2) * banks * width + bank * width + (word % words_per_bank) * 4;
}
static inline uint32_t test_float_bits(unsigned owner, unsigned index) {
    union { float f; uint32_t u; } bits;
    bits.f = (float)((owner * 32u + index) & 127u);
    return bits.u;
}
struct SharedBankTestResult {
    uint32_t magic, tile, status, phase;
    uint32_t verified_bytes, payload_checksum, mvm_words, mvm_checksum;
    uint32_t source_words, private_guard_bytes, error_code, detail0, detail1;
    uint32_t bank_width, router_bank0, router_bank1;
};
#endif
