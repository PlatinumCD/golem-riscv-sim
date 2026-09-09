#ifndef MITTENS_MEMORY_MAP_H
#define MITTENS_MEMORY_MAP_H

#include <stdint.h>

/* Shared QEMU/SST/guest ABI address. Capacity is a machine parameter. */
#define MITTENS_SCRATCHPAD_BASE UINT64_C(0x90000000)

#ifdef __cplusplus
#define MITTENS_MEMORY_CONSTEXPR constexpr
#else
#define MITTENS_MEMORY_CONSTEXPR
#endif

static inline MITTENS_MEMORY_CONSTEXPR int
mittens_memory_span_representable(uint64_t address, uint64_t length) {
    return length <= UINT64_MAX - address;
}

static inline MITTENS_MEMORY_CONSTEXPR int
mittens_memory_contains(uint64_t base, uint64_t capacity, uint64_t address) {
    return address >= base && address - base < capacity;
}

static inline MITTENS_MEMORY_CONSTEXPR int
mittens_memory_contains_range(uint64_t base, uint64_t capacity,
                             uint64_t address, uint64_t length) {
    return mittens_memory_span_representable(address, length) && address >= base &&
           address - base <= capacity && length <= capacity - (address - base);
}

#undef MITTENS_MEMORY_CONSTEXPR

#endif
