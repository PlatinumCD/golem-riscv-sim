#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

namespace {

constexpr uint32_t kTileId = MITTENS_TILE_ID;
constexpr uint32_t kWords = 1024;
constexpr uint32_t kBytes = kWords * sizeof(uint32_t);
constexpr uint64_t kGlobalOffset = 0;
constexpr uint64_t kExecution = 91;

uint32_t initialValue(uint32_t index) {
    return UINT32_C(0x6a000000) ^ index;
}

int fail(const char* message) {
    uart_puts(message);
    uart_putc('\n');
    return 1;
}

bool submitAndWait(
    uint32_t token,
    golem::platform::ScratchpadDMADirection direction
) {
    if (!golem::platform::globalDMASubmit(
            kGlobalOffset,
            0,
            kBytes,
            token,
            kExecution,
            token,
            direction)) {
        return false;
    }
    return golem::platform::globalDMAWait(kExecution, token);
}

bool arrive(uint32_t epoch, uint32_t contribution) {
    return mesh_nic::arrive_epoch(epoch, contribution);
}

}  // namespace

extern "C" int tile_main() {
    using golem::platform::ScratchpadDMADirection;
    auto* const words = reinterpret_cast<uint32_t*>(
        golem::platform::ScratchpadBase);

    if (kTileId == 0) {
        for (uint32_t index = 0; index < kWords; ++index) {
            words[index] = initialValue(index);
        }
        if (!submitAndWait(
                0, ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
            return fail("epoch barrier tile 0 seed DMA failed");
        }
        if (!arrive(0, mesh_nic::kEpochWorkComplete)) {
            return fail("epoch barrier tile 0 epoch 0 failed");
        }
        if (!arrive(1, mesh_nic::kEpochIdle)) {
            return fail("epoch barrier tile 0 epoch 1 failed");
        }
        if (!submitAndWait(
                1, ScratchpadDMADirection::GlobalRAMToScratchpad)) {
            return fail("epoch barrier tile 0 final DMA failed");
        }
        for (uint32_t index = 0; index < kWords; ++index) {
            if (words[index] != ~initialValue(index)) {
                return fail("epoch barrier tile 0 visibility mismatch");
            }
        }
        if (!arrive(2, mesh_nic::kEpochWorkComplete)) {
            return fail("epoch barrier tile 0 epoch 2 failed");
        }
        uart_puts("epoch barrier tile 0: PASS\n");
        return 0;
    }

    if (!arrive(0, mesh_nic::kEpochIdle)) {
        return fail("epoch barrier tile 1 epoch 0 failed");
    }
    if (!submitAndWait(
            0, ScratchpadDMADirection::GlobalRAMToScratchpad)) {
        return fail("epoch barrier tile 1 input DMA failed");
    }
    for (uint32_t index = 0; index < kWords; ++index) {
        if (words[index] != initialValue(index)) {
            return fail("epoch barrier tile 1 visibility mismatch");
        }
        words[index] = ~words[index];
    }
    if (!submitAndWait(
            1, ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
        return fail("epoch barrier tile 1 output DMA failed");
    }
    if (!arrive(1, mesh_nic::kEpochWorkComplete)) {
        return fail("epoch barrier tile 1 epoch 1 failed");
    }
    if (!arrive(2, mesh_nic::kEpochIdle)) {
        return fail("epoch barrier tile 1 epoch 2 failed");
    }
    uart_puts("epoch barrier tile 1: PASS\n");
    return 0;
}
