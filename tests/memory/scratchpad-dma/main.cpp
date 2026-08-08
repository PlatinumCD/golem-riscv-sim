#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

namespace {

alignas(64) uint32_t source[] = {
    UINT32_C(0x11223344),
    UINT32_C(0x55667788),
    UINT32_C(0x99aabbcc),
    UINT32_C(0xddeeff00),
    UINT32_C(0x01234567),
    UINT32_C(0x89abcdef),
    UINT32_C(0x13579bdf),
    UINT32_C(0x2468ace0),
};
alignas(64) uint32_t destination[8]{};

}  // namespace

extern "C" int tile_main() {
    using namespace golem::platform;
    auto* scratchpad0 = reinterpret_cast<uint32_t*>(ScratchpadBase + 64);
    auto* scratchpad1 = reinterpret_cast<uint32_t*>(ScratchpadBase + 128);

    scratchpadDMASubmit(
        source,
        scratchpad0,
        4 * sizeof(uint32_t),
        0,
        0,
        ScratchpadDMADirection::BackingToScratchpad);
    scratchpadDMASubmit(
        source + 4,
        scratchpad1,
        4 * sizeof(uint32_t),
        1,
        0,
        ScratchpadDMADirection::BackingToScratchpad);
    if (!scratchpadDMAWait(0, 0) || !scratchpadDMAWait(0, 1)) {
        uart_puts("scratchpad DMA input failed\n");
        return 1;
    }
    for (uint32_t index = 0; index < 4; ++index) {
        scratchpad0[index] ^= UINT32_C(0xffffffff);
        scratchpad1[index] ^= UINT32_C(0xffffffff);
    }
    scratchpadDMASubmit(
        scratchpad0,
        destination,
        4 * sizeof(uint32_t),
        2,
        0,
        ScratchpadDMADirection::ScratchpadToBacking);
    scratchpadDMASubmit(
        scratchpad1,
        destination + 4,
        4 * sizeof(uint32_t),
        3,
        0,
        ScratchpadDMADirection::ScratchpadToBacking);
    if (!scratchpadDMAWait(0, 2) || !scratchpadDMAWait(0, 3)) {
        uart_puts("scratchpad DMA output failed\n");
        return 1;
    }
    for (uint32_t index = 0; index < 8; ++index) {
        if (destination[index] != (source[index] ^ UINT32_C(0xffffffff))) {
            uart_puts("scratchpad DMA data mismatch\n");
            return 1;
        }
    }
    uart_puts("scratchpad DMA test: PASS\n");
    return 0;
}
