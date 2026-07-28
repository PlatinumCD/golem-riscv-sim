#include <stddef.h>
#include <stdint.h>

extern "C" char __heap_start[];
extern "C" char __heap_end[];

namespace {

constexpr uintptr_t kAlignment = 64;
uintptr_t next_allocation =
    reinterpret_cast<uintptr_t>(__heap_start);

uintptr_t alignUp(uintptr_t value) {
    return (value + kAlignment - 1U) & ~(kAlignment - 1U);
}

}  // namespace

extern "C" void* malloc(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    const uintptr_t begin = alignUp(next_allocation);
    const uintptr_t end = begin + size;
    if (end < begin ||
        end > reinterpret_cast<uintptr_t>(__heap_end)) {
        return nullptr;
    }

    next_allocation = end;
    return reinterpret_cast<void*>(begin);
}

extern "C" void free(void*) {
    // This fixed one-execution proof uses a monotonic per-tile heap.
}

extern "C" void* memcpy(void* destination, const void* source, size_t size) {
    auto* output = static_cast<unsigned char*>(destination);
    const auto* input = static_cast<const unsigned char*>(source);
    for (size_t index = 0; index < size; ++index) {
        output[index] = input[index];
    }
    return destination;
}
