/* Plain C: LLVM chooses vector width, grouping, instructions and scheduling. */
#include <stddef.h>
#include <stdint.h>

__attribute__((noinline))
uint32_t transfer_kernel(const uint32_t *restrict x,
                         const uint32_t *restrict y,
                         uint32_t *restrict out, size_t count)
{
#if APPLICATION == 0
    for (size_t i = 0; i < count; ++i)
        out[i] = x[i];
    return 0;
#elif APPLICATION == 1
    for (size_t i = 0; i < count; ++i)
        out[i] = x[i] + y[i];
    return 0;
#elif APPLICATION == 2
    uint32_t result = 0;
    for (size_t i = 0; i < count; ++i)
        result += x[i];
    return result;
#elif APPLICATION == 3
    /* x contains count+2 words: the first and last are the two halos. */
    for (size_t i = 0; i < count; ++i)
        out[i] = x[i] + x[i + 1] + x[i + 2];
    return 0;
#else
#error Unsupported APPLICATION
#endif
}
