#pragma once

#include <stdint.h>

namespace mesh_nic {

constexpr uintptr_t kBase = 0x10010000UL;
constexpr uint32_t kTransmitReady = 1U << 0;
constexpr uint32_t kReceiveValid = 1U << 1;
constexpr uint32_t kTransmitBurstReady = 1U << 2;
constexpr uint32_t kBurstWordCapacity = 4096;
constexpr uint32_t kReceiveDmaSubmitReady = 1U << 0;
constexpr uint32_t kReceiveDmaCompletionValid = 1U << 1;
constexpr uint32_t kTaskTraceStart = 1;
constexpr uint32_t kTaskTraceFinish = 2;
constexpr uint32_t kMemoryInitializationComplete = 3;
constexpr uint32_t kEpochBarrierArrive = 4;
constexpr uint32_t kEpochWorkComplete = 0;
constexpr uint32_t kEpochIdle = 1;

inline volatile uint32_t* registers() {
    return reinterpret_cast<volatile uint32_t*>(kBase);
}

inline uint32_t status() {
    return registers()[0];
}

inline bool try_send(uint32_t destination, uint32_t payload) {
    if ((status() & kTransmitReady) == 0) {
        return false;
    }

    registers()[1] = destination;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[2] = payload;
    return true;
}

inline void send(uint32_t destination, uint32_t payload) {
    while (!try_send(destination, payload)) {
    }
}

inline bool try_send_words(
    uint32_t destination,
    const uint32_t* words,
    uint32_t word_count
) {
    if (words == nullptr ||
        word_count == 0 ||
        word_count > kBurstWordCapacity ||
        (status() & kTransmitBurstReady) == 0) {
        return false;
    }

    const uint64_t address =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(words));
    registers()[1] = destination;
    registers()[5] = static_cast<uint32_t>(address);
    registers()[6] = static_cast<uint32_t>(address >> 32U);
    registers()[7] = word_count;
    __asm__ volatile("fence rw, iorw" ::: "memory");
    registers()[8] = 1;
    return true;
}

inline bool try_receive(uint32_t* payload) {
    if (payload == nullptr || (status() & kReceiveValid) == 0) {
        return false;
    }

    *payload = registers()[3];
    return true;
}

inline bool try_receive_from(uint32_t* source, uint32_t* payload) {
    if (source == nullptr ||
        payload == nullptr ||
        (status() & kReceiveValid) == 0) {
        return false;
    }

    *source = registers()[4];
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    *payload = registers()[3];
    return true;
}

inline uint32_t receive_dma_status() {
    return registers()[20];
}

inline bool try_start_receive_words(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    void* destination,
    uint32_t word_count
) {
    if (destination == nullptr ||
        word_count == 0 ||
        (receive_dma_status() & kReceiveDmaSubmitReady) == 0) {
        return false;
    }

    const uint64_t address =
        static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(destination));
    registers()[14] = source;
    registers()[15] = route_id;
    registers()[16] = static_cast<uint32_t>(address);
    registers()[17] = static_cast<uint32_t>(address >> 32U);
    registers()[18] = word_count;
    registers()[25] = static_cast<uint32_t>(logical_iteration);
    registers()[26] = static_cast<uint32_t>(logical_iteration >> 32U);
    __asm__ volatile("fence rw, iorw" ::: "memory");
    registers()[19] = 1;
    return true;
}

inline bool try_claim_receive_words(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint32_t word_count
) {
    if (word_count == 0) {
        return false;
    }

    registers()[14] = source;
    registers()[15] = route_id;
    registers()[18] = word_count;
    registers()[25] = static_cast<uint32_t>(logical_iteration);
    registers()[26] = static_cast<uint32_t>(logical_iteration >> 32U);
    __asm__ volatile("fence rw, iorw" ::: "memory");
    registers()[29] = 1;
    return true;
}

inline bool try_receive_words_completion(
    uint32_t* source,
    uint32_t* route_id,
    uint64_t* logical_iteration
) {
    if (source == nullptr ||
        route_id == nullptr ||
        logical_iteration == nullptr ||
        (receive_dma_status() & kReceiveDmaCompletionValid) == 0) {
        return false;
    }

    *source = registers()[21];
    *route_id = registers()[22];
    *logical_iteration = registers()[27];
    *logical_iteration |= static_cast<uint64_t>(registers()[28]) << 32U;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[23] = 1;
    return true;
}

inline void wait_for_receive() {
    if ((status() & kReceiveValid) != 0 ||
        (receive_dma_status() & kReceiveDmaCompletionValid) != 0) {
        return;
    }

    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[9] = 1;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

inline void wait_for_transmit() {
    if ((status() & kTransmitBurstReady) != 0) {
        return;
    }

    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[24] = 1;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

inline void trace_task(
    uint32_t event,
    uint32_t task_id,
    uint64_t execution_id
) {
    registers()[10] = task_id;
    registers()[11] = static_cast<uint32_t>(execution_id);
    registers()[12] = static_cast<uint32_t>(execution_id >> 32);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[13] = event;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

inline void complete_memory_initialization() {
    __asm__ volatile("fence rw, iorw" ::: "memory");
    registers()[13] = kMemoryInitializationComplete;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

inline bool arrive_epoch(uint32_t completed_epoch, uint32_t contribution) {
    if (contribution != kEpochWorkComplete &&
        contribution != kEpochIdle) {
        return false;
    }
    trace_task(kEpochBarrierArrive, completed_epoch, contribution);
    return true;
}

inline uint32_t receive() {
    uint32_t payload = 0;
    while (!try_receive(&payload)) {
        wait_for_receive();
    }

    return payload;
}

inline uint32_t receive_from(uint32_t* source) {
    uint32_t payload = 0;
    while (!try_receive_from(source, &payload)) {
        wait_for_receive();
    }
    return payload;
}

} // namespace mesh_nic
