#include <stddef.h>
#include <stdint.h>

extern uint32_t run_phase(uint32_t phase, size_t count,
    const uint32_t *x, const uint32_t *y, uint32_t *out);

int main(void)
{
    const uint32_t *const x = (const uint32_t *)(uintptr_t)0x90100000;
    const uint32_t *const y = (const uint32_t *)(uintptr_t)0x90200000;
    uint32_t *const out = (uint32_t *)(uintptr_t)0x90300000;
    uint32_t *const warm_out = (uint32_t *)(uintptr_t)0x90400000;
    volatile uint32_t *const mailbox = (volatile uint32_t *)(uintptr_t)0x90500000;
    uint32_t warm = run_phase(2, ELEMENT_COUNT, x, y, warm_out);
    run_phase(0, 0, x, y, out);
    uint32_t measured = run_phase(1, ELEMENT_COUNT, x, y, out);
    /* Full host oracle is outside the timed phases; make both results visible. */
    mailbox[0] = warm;
    mailbox[1] = measured;
    mailbox[2] = UINT32_C(0xa991ca7e);
    return warm != measured;
}
