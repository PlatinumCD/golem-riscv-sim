#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

namespace {

constexpr uint32_t DMAWindow = 8;
constexpr uint32_t TransferCount = DMAWindow + 1;
constexpr uint32_t WordsPerTransfer = 4;
constexpr uint64_t TransferStride = 64;
constexpr uint64_t InitializationGlobalOffset = 4096;
constexpr uint64_t InitializationScratchpadOffset = 2048;
constexpr uint32_t GroupTransferCount = 4;
constexpr uint64_t GroupGlobalOffset = 8192;
constexpr uint64_t GroupScratchpadOffset = 4096;

uint32_t expected(uint32_t transfer, uint32_t word) {
    return UINT32_C(0x10203040) ^ (transfer << 16U) ^ word;
}

uint32_t initializationExpected(uint32_t word) {
    return UINT32_C(0xa5c30000) ^ word;
}

uint32_t groupExpected(uint32_t transfer, uint32_t word) {
    return UINT32_C(0x5a710000) ^ (transfer << 12U) ^ word;
}
}  // namespace

extern "C" int tile_main() {
    using namespace golem::platform;

    auto* initialization = reinterpret_cast<uint32_t*>(
        ScratchpadBase + InitializationScratchpadOffset);
    for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
        initialization[word] = initializationExpected(word);
    }
    if (!globalDMASubmit(
            InitializationGlobalOffset,
            InitializationScratchpadOffset,
            WordsPerTransfer * sizeof(uint32_t),
            UINT32_C(0x101), UINT64_C(2), 0,
            ScratchpadDMADirection::ScratchpadToGlobalRAM) ||
        !globalDMAWait(UINT64_C(2), UINT32_C(0x101))) {
        uart_puts("global RAM initialization failed\n");
        return 1;
    }
    for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
        initialization[word] = 0;
    }
    if (!globalDMASubmit(
            InitializationGlobalOffset,
            InitializationScratchpadOffset,
            WordsPerTransfer * sizeof(uint32_t),
            UINT32_C(0x100),
            UINT64_C(2),
            0,
            ScratchpadDMADirection::GlobalRAMToScratchpad) ||
        !globalDMAWait(UINT64_C(2), UINT32_C(0x100))) {
        uart_puts("global RAM initialized readback failed\n");
        return 1;
    }
    for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
        if (initialization[word] != initializationExpected(word)) {
            uart_puts("global RAM initialized data mismatch\n");
            return 1;
        }
    }

    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + TransferStride * (transfer + 1U));
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = expected(transfer, word);
        }
    }

    // Submit the endpoint's full legal window before waiting.  This is the
    // contract the SST controller's per-tile queue must be able to retain.
    for (uint32_t transfer = 0; transfer < DMAWindow; ++transfer) {
        if (!globalDMASubmit(
            TransferStride * transfer,
            TransferStride * (transfer + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            transfer,
            0,
            transfer,
            ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
            uart_puts("global DMA window submit failed\n");
            return 1;
        }
    }
    if (globalDMASubmit(
            TransferStride * DMAWindow,
            TransferStride * (DMAWindow + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            DMAWindow,
            0,
            DMAWindow,
            ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
        uart_puts("global DMA exceeded endpoint window\n");
        return 1;
    }
    if (!globalDMAWait(0, 0) ||
        !globalDMASubmit(
            TransferStride * DMAWindow,
            TransferStride * (DMAWindow + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            DMAWindow,
            0,
            DMAWindow,
            ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
        uart_puts("global DMA backpressure retry failed\n");
        return 1;
    }
    for (uint32_t transfer = 1; transfer < TransferCount; ++transfer) {
        if (!globalDMAWait(0, transfer)) {
            uart_puts("global DMA seed failed\n");
            return 1;
        }
    }

    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + TransferStride * (transfer + 1U));
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = 0;
        }
        const uint32_t token = TransferCount + transfer;
        if (transfer < DMAWindow && !globalDMASubmit(
            TransferStride * transfer,
            TransferStride * (transfer + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            token,
            1,
            token,
            ScratchpadDMADirection::GlobalRAMToScratchpad)) {
            uart_puts("global DMA read window submit failed\n");
            return 1;
        }
    }
    const uint32_t ninth_read_token = TransferCount + DMAWindow;
    if (globalDMASubmit(
            TransferStride * DMAWindow,
            TransferStride * (DMAWindow + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            ninth_read_token,
            1,
            ninth_read_token,
            ScratchpadDMADirection::GlobalRAMToScratchpad)) {
        uart_puts("global DMA read exceeded endpoint window\n");
        return 1;
    }
    if (!globalDMAWaitBatch(1, TransferCount, DMAWindow) ||
        !globalDMASubmit(
            TransferStride * DMAWindow,
            TransferStride * (DMAWindow + 1U),
            WordsPerTransfer * sizeof(uint32_t),
            ninth_read_token,
            1,
            ninth_read_token,
            ScratchpadDMADirection::GlobalRAMToScratchpad)) {
        uart_puts("global DMA read backpressure retry failed\n");
        return 1;
    }
    if (!globalDMAWait(1, ninth_read_token)) {
        uart_puts("global DMA verify tail read failed\n");
        return 1;
    }
    for (uint32_t transfer = 0; transfer < TransferCount; ++transfer) {
        const auto* scratchpad = reinterpret_cast<const uint32_t*>(
            ScratchpadBase + TransferStride * (transfer + 1U));
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            if (scratchpad[word] != expected(transfer, word)) {
                uart_puts("global DMA data mismatch\n");
                return 1;
            }
        }
    }

    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + GroupScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = groupExpected(transfer, word);
        }
    }
    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        if (!globalDMASubmit(
                GroupGlobalOffset + TransferStride * transfer,
                GroupScratchpadOffset + TransferStride * transfer,
                WordsPerTransfer * sizeof(uint32_t),
                UINT32_C(0x200) + transfer,
                UINT64_C(3),
                transfer,
                ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
            uart_puts("global DMA write group submit failed\n");
            return 1;
        }
    }
    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        if (!globalDMAWait(UINT64_C(3), UINT32_C(0x200) + transfer)) {
            uart_puts("global DMA write group wait failed\n");
            return 1;
        }
    }

    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + GroupScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = 0;
        }
    }
    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        if (!globalDMASubmit(
                GroupGlobalOffset + TransferStride * transfer,
                GroupScratchpadOffset + TransferStride * transfer,
                WordsPerTransfer * sizeof(uint32_t),
                UINT32_C(0x300),
                UINT64_C(4),
                transfer,
                ScratchpadDMADirection::GlobalRAMToScratchpad)) {
            uart_puts("global DMA read group submit failed\n");
            return 1;
        }
        // A completed token can be reused by the next descriptor.
        if (!globalDMAWait(UINT64_C(4), UINT32_C(0x300))) {
            uart_puts("global DMA read group wait failed\n");
            return 1;
        }
    }
    for (uint32_t transfer = 0; transfer < GroupTransferCount; ++transfer) {
        const auto* scratchpad = reinterpret_cast<const uint32_t*>(
            ScratchpadBase + GroupScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            if (scratchpad[word] != groupExpected(transfer, word)) {
                uart_puts("global DMA group data mismatch\n");
                return 1;
            }
        }
    }
    uart_puts("global DMA test: PASS\n");
    return 0;
}
