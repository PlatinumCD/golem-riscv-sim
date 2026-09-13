#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

namespace {

constexpr uint32_t TileId = MITTENS_TILE_ID;
constexpr uint32_t TransferCount = 2;
constexpr uint32_t TransferBytes = 4096;
constexpr uint64_t GlobalOffset = 65536;
constexpr uint64_t Execution = 97;
constexpr uint32_t Exact =
    golem::platform::ScratchpadDMAExactReadiness;
constexpr uint32_t Teardown =
    Exact | golem::platform::ScratchpadDMAExactExecutionTeardown;

uint8_t expectedByte(uint32_t transfer, uint32_t index) {
    return static_cast<uint8_t>(
        UINT32_C(0x5b) ^ (transfer * 73U) ^ (index * 29U) ^ (index >> 4U));
}

int fail(const char* message) {
    uart_puts(message);
    uart_putc('\n');
    return 1;
}

void delayProducer() {
    volatile uint32_t state = UINT32_C(0x12345678);
    for (uint32_t iteration = 0; iteration < 50000; ++iteration) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    }
    if (state == 0) {
        uart_putc(' ');
    }
}

bool submit(uint32_t transfer,
            golem::platform::ScratchpadDMADirection direction) {
    return golem::platform::globalDMASubmit(
        GlobalOffset + static_cast<uint64_t>(transfer) * TransferBytes,
        static_cast<uint64_t>(transfer) * TransferBytes,
        TransferBytes,
        transfer,
        Execution,
        transfer,
        direction,
        Exact);
}

bool wait(uint32_t transfer) {
    return golem::platform::globalDMAWait(Execution, transfer);
}

bool teardown() {
    constexpr uint32_t token = UINT32_MAX;
    return golem::platform::globalDMASubmit(
               0,
               0,
               0,
               token,
               Execution,
               UINT64_MAX,
               golem::platform::ScratchpadDMADirection::ScratchpadToGlobalRAM,
               Teardown) &&
           golem::platform::globalDMAWait(Execution, token);
}

int runProducer() {
    using Direction = golem::platform::ScratchpadDMADirection;
    auto* const scratchpad = reinterpret_cast<uint8_t*>(
        golem::platform::ScratchpadBase);
    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        for (uint32_t index = 0; index < TransferBytes; ++index) {
            scratchpad[transfer * TransferBytes + index] =
                expectedByte(transfer, index);
        }
    }

    // Let the consumer's exact reads reach the shared controller first.
    delayProducer();
    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        if (!submit(transfer, Direction::ScratchpadToGlobalRAM)) {
            return fail("DMA contention producer submit failed");
        }
    }
    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        if (!wait(transfer)) {
            return fail("DMA contention producer wait failed");
        }
    }
    if (!teardown()) {
        return fail("DMA contention producer teardown failed");
    }
    uart_puts("DMA contention tile 0: PASS\n");
    return 0;
}

int runConsumer() {
    using Direction = golem::platform::ScratchpadDMADirection;
    auto* const scratchpad = reinterpret_cast<uint8_t*>(
        golem::platform::ScratchpadBase);
    for (uint32_t index = 0;
         index < TransferCount * TransferBytes;
         ++index) {
        scratchpad[index] = 0;
    }

    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        if (!submit(transfer, Direction::GlobalRAMToScratchpad)) {
            return fail("DMA contention consumer submit failed");
        }
    }
    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        if (!wait(transfer)) {
            return fail("DMA contention consumer wait failed");
        }
    }

    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        for (uint32_t index = 0; index < TransferBytes; ++index) {
            if (scratchpad[transfer * TransferBytes + index] !=
                expectedByte(transfer, index)) {
                return fail("DMA contention payload mismatch");
            }
        }
    }
    if (!teardown()) {
        return fail("DMA contention consumer teardown failed");
    }
    uart_puts("DMA contention tile 1: PASS\n");
    return 0;
}

}  // namespace

extern "C" int tile_main() {
    if (TileId == 0) {
        return runProducer();
    }
    if (TileId == 1) {
        return runConsumer();
    }
    return fail("DMA contention invalid tile ID");
}
