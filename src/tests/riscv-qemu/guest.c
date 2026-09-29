#include <stdint.h>

#define BASE ((uintptr_t)0x90100000)
#define START (*(volatile uint32_t *)(BASE + 0x00))
#define READY (*(volatile uint32_t *)(BASE + 0x40))
#define ACK (*(volatile uint32_t *)(BASE + 0x80))
#define START_VALUE 0x1badb002u
#define READY_VALUE 0xc001c0deu
#define ACK_VALUE 0x600d600du

static volatile uint64_t initialized_data = UINT64_C(0x1122334455667788);
static volatile uint32_t zero_bss[17];

int main(void)
{
    volatile uint32_t stack_data[17];
    if (initialized_data != UINT64_C(0x1122334455667788)) return 1;
    for (unsigned i = 0; i < 17; ++i) {
        if (zero_bss[i] != 0) return 2;
        stack_data[i] = 3 * i + 1;
        zero_bss[i] = 7 * i + 5;
    }
    for (unsigned i = 0; i < 17; ++i)
        if (stack_data[i] != 3 * i + 1 || zero_bss[i] != 7 * i + 5) return 3;

    unsigned attempts = 0;
    while (START != START_VALUE) if (++attempts == 100000) return 4;
    volatile uint32_t *input = (volatile uint32_t *)(BASE + 0x04);
    volatile uint32_t *output = (volatile uint32_t *)(BASE + 0x44);
    for (unsigned i = 0; i < 8; ++i)
        if (input[i] != 0x100 + 3 * i) return 5;
    int (*shared_code)(void) = (int (*)(void))(BASE + 0xa0);
    __asm__ volatile ("fence.i" : : : "memory");
    if (shared_code() != 17) return 9;

    // Both vector transfers cross a 32-byte request boundary. At request_bytes=4,
    // the scalar 64-bit accesses also require multiple independently timed pieces.
    __asm__ volatile (
        "vsetivli zero, 8, e32, m1, ta, ma\n\t"
        "vle32.v v1, (%0)\n\t"
        "li t0, 17\n\t"
        "vadd.vx v2, v1, t0\n\t"
        "vse32.v v2, (%1)\n\t"
        : : "r" (input), "r" (output) : "t0", "v1", "v2", "memory");
    __asm__ volatile ("fence rw, rw\n\tfence.i" : : : "memory");
    for (unsigned i = 0; i < 8; ++i)
        if (output[i] != 0x111 + 3 * i) return 6;
    volatile uint64_t *scalar = (volatile uint64_t *)(BASE + 0x68);
    *scalar = initialized_data;
    if (*scalar != initialized_data) return 7;
    __asm__ volatile ("fence rw, rw" : : : "memory");
    READY = READY_VALUE;

    attempts = 0;
    while (ACK != ACK_VALUE) if (++attempts == 100000) return 8;
    // The peer changes code already executed by this CPU before sending ACK.
    __asm__ volatile ("fence.i" : : : "memory");
    if (shared_code() != 29) return 10;
    return 0;
}
