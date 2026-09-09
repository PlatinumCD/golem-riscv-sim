#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

namespace {

constexpr uint32_t kTileId = MITTENS_TILE_ID;
constexpr uint32_t kPeerId = 1U - kTileId;
constexpr uint32_t kWords = 1024;
constexpr uint32_t kBytes = kWords * sizeof(uint32_t);
constexpr uint64_t kDisjointOffsets[2] = {0, 4096};
constexpr uint64_t kSharedOffset = 8192;
constexpr uint64_t kExecution = 17;

uint32_t disjointValue(uint32_t tile, uint32_t index) {
    return UINT32_C(0x51000000) ^ (tile << 20U) ^ index;
}

uint32_t sharedValue(uint32_t index) {
    return UINT32_C(0xa5300000) ^ index;
}

void sendBarrier(uint32_t value) {
    while (!mesh_nic::try_send(kPeerId, value)) {
        mesh_nic::wait_for_transmit();
    }
}

bool receiveBarrier(uint32_t value) {
    uint32_t source = UINT32_MAX;
    return mesh_nic::receive_from(&source) == value && source == kPeerId;
}

bool waitDMA(uint32_t token) {
    return golem::platform::globalDMAWait(kExecution, token);
}

void submitDMA(uint64_t global_offset, uint64_t scratchpad_offset,
               uint32_t token,
               golem::platform::ScratchpadDMADirection direction) {
    golem::platform::globalDMASubmit(
        global_offset, scratchpad_offset, kBytes, token, kExecution, token,
        direction);
}

int fail(const char* message) {
    uart_puts(message);
    uart_putc('\n');
    return 1;
}

}  // namespace

extern "C" int tile_main() {
    using golem::platform::ScratchpadDMADirection;
    auto* local = reinterpret_cast<uint32_t*>(golem::platform::ScratchpadBase);
    auto* remote = reinterpret_cast<uint32_t*>(
        golem::platform::ScratchpadBase + kBytes);

    sendBarrier(UINT32_C(0x100));
    if (!receiveBarrier(UINT32_C(0x100)))
        return fail("global RAM initial barrier failed");

    for (uint32_t index = 0; index < kWords; ++index)
        local[index] = disjointValue(kTileId, index);
    submitDMA(kDisjointOffsets[kTileId], 0, 0,
              ScratchpadDMADirection::ScratchpadToGlobalRAM);
    if (!waitDMA(0))
        return fail("global RAM disjoint write failed");

    sendBarrier(UINT32_C(0x101));
    if (!receiveBarrier(UINT32_C(0x101)))
        return fail("global RAM disjoint barrier failed");

    submitDMA(kDisjointOffsets[kPeerId], kBytes, 1,
              ScratchpadDMADirection::GlobalRAMToScratchpad);
    if (!waitDMA(1))
        return fail("global RAM peer read failed");
    for (uint32_t index = 0; index < kWords; ++index) {
        if (remote[index] != disjointValue(kPeerId, index))
            return fail("global RAM disjoint data mismatch");
    }

    sendBarrier(UINT32_C(0x102));
    if (!receiveBarrier(UINT32_C(0x102)))
        return fail("global RAM shared barrier failed");

    if (kTileId == 0) {
        for (uint32_t index = 0; index < kWords; ++index)
            local[index] = sharedValue(index);
        submitDMA(kSharedOffset, 0, 2,
                  ScratchpadDMADirection::ScratchpadToGlobalRAM);
        if (!waitDMA(2))
            return fail("global RAM shared seed failed");
        sendBarrier(UINT32_C(0x103));
        if (!receiveBarrier(UINT32_C(0x104)))
            return fail("global RAM shared response failed");
        submitDMA(kSharedOffset, 0, 3,
                  ScratchpadDMADirection::GlobalRAMToScratchpad);
        if (!waitDMA(3))
            return fail("global RAM shared final read failed");
        for (uint32_t index = 0; index < kWords; ++index) {
            if (local[index] !=
                (sharedValue(index) ^ UINT32_C(0xffffffff)))
                return fail("global RAM shared final mismatch");
        }
    } else {
        if (!receiveBarrier(UINT32_C(0x103)))
            return fail("global RAM shared seed notification failed");
        submitDMA(kSharedOffset, 0, 2,
                  ScratchpadDMADirection::GlobalRAMToScratchpad);
        if (!waitDMA(2))
            return fail("global RAM shared tile1 read failed");
        for (uint32_t index = 0; index < kWords; ++index) {
            if (local[index] != sharedValue(index))
                return fail("global RAM shared seed mismatch");
            local[index] ^= UINT32_C(0xffffffff);
        }
        submitDMA(kSharedOffset, 0, 3,
                  ScratchpadDMADirection::ScratchpadToGlobalRAM);
        if (!waitDMA(3))
            return fail("global RAM shared tile1 write failed");
        sendBarrier(UINT32_C(0x104));
    }

    uart_puts(kTileId == 0 ? "global RAM tile 0: PASS\n"
                           : "global RAM tile 1: PASS\n");
    return 0;
}
