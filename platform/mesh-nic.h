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

inline void send(uint32_t destination, uint32_t payload) {
    while ((status() & kTransmitReady) == 0) {
    }

    registers()[1] = destination;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    registers()[2] = payload;
}

inline uint32_t receive() {
    while ((status() & kReceiveValid) == 0) {
    }

    return registers()[3];
}

} // namespace mesh_nic
