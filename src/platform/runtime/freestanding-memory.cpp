#include <stddef.h>
#include <stdint.h>

extern "C" char __heap_start[];
extern "C" char __heap_end[];

namespace {

constexpr uintptr_t kAlignment = 64;
typedef uint64_t AliasedUInt64 __attribute__((__may_alias__));
typedef uint32_t AliasedUInt32 __attribute__((__may_alias__));

uintptr_t alignUp(uintptr_t value) {
    return (value + kAlignment - 1U) & ~(kAlignment - 1U);
}

struct alignas(kAlignment) Block {
    size_t size;
    Block* previous;
    Block* next;
    bool available;
};

static_assert(sizeof(Block) == kAlignment);

Block* first_block = nullptr;

void initializeHeap() {
    if (first_block != nullptr) {
        return;
    }
    const uintptr_t begin =
        alignUp(reinterpret_cast<uintptr_t>(__heap_start));
    const uintptr_t end =
        reinterpret_cast<uintptr_t>(__heap_end) &
        ~(kAlignment - 1U);
    if (end <= begin || end - begin <= sizeof(Block)) {
        return;
    }
    first_block = reinterpret_cast<Block*>(begin);
    first_block->size = end - begin - sizeof(Block);
    first_block->previous = nullptr;
    first_block->next = nullptr;
    first_block->available = true;
}

void mergeWithNext(Block* block) {
    Block* next = block->next;
    if (next == nullptr || !next->available) {
        return;
    }
    block->size += sizeof(Block) + next->size;
    block->next = next->next;
    if (block->next != nullptr) {
        block->next->previous = block;
    }
}

}  // namespace

extern "C" void* malloc(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    initializeHeap();
    if (first_block == nullptr ||
        size > SIZE_MAX - (kAlignment - 1U)) {
        return nullptr;
    }
    const size_t aligned_size =
        (size + kAlignment - 1U) & ~(kAlignment - 1U);

    for (Block* block = first_block;
         block != nullptr;
         block = block->next) {
        if (!block->available || block->size < aligned_size) {
            continue;
        }
        if (block->size >=
            aligned_size + sizeof(Block) + kAlignment) {
            auto* remainder = reinterpret_cast<Block*>(
                reinterpret_cast<uint8_t*>(block + 1) + aligned_size
            );
            remainder->size =
                block->size - aligned_size - sizeof(Block);
            remainder->previous = block;
            remainder->next = block->next;
            remainder->available = true;
            if (remainder->next != nullptr) {
                remainder->next->previous = remainder;
            }
            block->size = aligned_size;
            block->next = remainder;
        }
        block->available = false;
        return block + 1;
    }
    return nullptr;
}

extern "C" void free(void* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* block = static_cast<Block*>(memory) - 1;
    block->available = true;
    mergeWithNext(block);
    if (block->previous != nullptr && block->previous->available) {
        block = block->previous;
        mergeWithNext(block);
    }
}

extern "C" void* memcpy(void* destination, const void* source, size_t size) {
    auto* output = static_cast<unsigned char*>(destination);
    const auto* input = static_cast<const unsigned char*>(source);
    if ((reinterpret_cast<uintptr_t>(output) |
         reinterpret_cast<uintptr_t>(input)) %
            alignof(AliasedUInt64) ==
        0) {
        while (size >= sizeof(AliasedUInt64)) {
            *reinterpret_cast<AliasedUInt64*>(output) =
                *reinterpret_cast<const AliasedUInt64*>(input);
            output += sizeof(AliasedUInt64);
            input += sizeof(AliasedUInt64);
            size -= sizeof(AliasedUInt64);
        }
    }
    if ((reinterpret_cast<uintptr_t>(output) |
         reinterpret_cast<uintptr_t>(input)) %
            alignof(AliasedUInt32) ==
        0) {
        while (size >= sizeof(AliasedUInt32)) {
            *reinterpret_cast<AliasedUInt32*>(output) =
                *reinterpret_cast<const AliasedUInt32*>(input);
            output += sizeof(AliasedUInt32);
            input += sizeof(AliasedUInt32);
            size -= sizeof(AliasedUInt32);
        }
    }
    while (size != 0) {
        *output++ = *input++;
        --size;
    }
    return destination;
}

extern "C" void* memmove(void* destination, const void* source, size_t size) {
    auto* output = static_cast<unsigned char*>(destination);
    const auto* input = static_cast<const unsigned char*>(source);

    if (output == input || size == 0) {
        return destination;
    }
    if (output < input || output >= input + size) {
        return memcpy(destination, source, size);
    }

    output += size;
    input += size;
    while (size != 0) {
        *--output = *--input;
        --size;
    }
    return destination;
}

extern "C" void* memset(void* destination, int value, size_t size) {
    auto* output = static_cast<unsigned char*>(destination);
    const uint8_t byte = static_cast<uint8_t>(value);
    const AliasedUInt64 value64 =
        static_cast<AliasedUInt64>(byte) *
        UINT64_C(0x0101010101010101);
    const AliasedUInt32 value32 =
        static_cast<AliasedUInt32>(byte) *
        UINT32_C(0x01010101);
    if (reinterpret_cast<uintptr_t>(output) %
            alignof(AliasedUInt64) ==
        0) {
        while (size >= sizeof(AliasedUInt64)) {
            *reinterpret_cast<AliasedUInt64*>(output) = value64;
            output += sizeof(AliasedUInt64);
            size -= sizeof(AliasedUInt64);
        }
    }
    if (reinterpret_cast<uintptr_t>(output) %
            alignof(AliasedUInt32) ==
        0) {
        while (size >= sizeof(AliasedUInt32)) {
            *reinterpret_cast<AliasedUInt32*>(output) = value32;
            output += sizeof(AliasedUInt32);
            size -= sizeof(AliasedUInt32);
        }
    }
    while (size != 0) {
        *output++ = byte;
        --size;
    }
    return destination;
}

extern "C" void* calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) {
        return nullptr;
    }
    if (size > SIZE_MAX / count) {
        return nullptr;
    }

    const size_t allocation_size = count * size;
    void* memory = malloc(allocation_size);
    if (memory == nullptr) {
        return nullptr;
    }
    return memset(memory, 0, allocation_size);
}
