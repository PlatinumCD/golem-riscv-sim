#pragma once

#include <stdint.h>

namespace mesh_nic {

constexpr uintptr_t kBase = 0x10010000UL;
constexpr uint32_t kTransmitReady = 1U << 0;
constexpr uint32_t kReceiveValid = 1U << 1;

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

inline bool try_receive(uint32_t* payload) {
    if (payload == nullptr || (status() & kReceiveValid) == 0) {
        return false;
    }

    *payload = registers()[3];
    return true;
}

inline uint32_t receive() {
    uint32_t payload = 0;
    while (!try_receive(&payload)) {
    }

    return payload;
}

} // namespace mesh_nic
