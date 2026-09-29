#include <stdint.h>

int main(void)
{
#if NEGATIVE_KIND == 1
    return 7;
#elif NEGATIVE_KIND == 2
    // Scratchpad boot intentionally has no DRAM mapping. This faults and
    // enters an invalid trap handler at address zero; the CPU must terminate
    // with a diagnostic instead of granting an endless untimed trap loop.
    return *(volatile uint64_t *)(uintptr_t)0x80000000;
#else
#error Unknown negative test
#endif
}
