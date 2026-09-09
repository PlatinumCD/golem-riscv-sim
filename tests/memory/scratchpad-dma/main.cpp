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
constexpr uint32_t MacroTransferCount = 4;
constexpr uint64_t MacroGlobalOffset = 8192;
constexpr uint64_t MacroScratchpadOffset = 4096;
constexpr uint64_t MacroInstructionBound = 65536;

uint32_t expected(uint32_t transfer, uint32_t word) {
    return UINT32_C(0x10203040) ^ (transfer << 16U) ^ word;
}

uint32_t initializationExpected(uint32_t word) {
    return UINT32_C(0xa5c30000) ^ word;
}

uint32_t macroExpected(uint32_t transfer, uint32_t word) {
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
    if (!globalRAMInitialize(
            InitializationGlobalOffset,
            InitializationScratchpadOffset,
            WordsPerTransfer * sizeof(uint32_t))) {
        uart_puts("global RAM initialization failed\n");
        return 1;
    }
    for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
        initialization[word] = 0;
    }
    mesh_nic::complete_memory_initialization();
    if (globalRAMInitialize(
            InitializationGlobalOffset,
            InitializationScratchpadOffset,
            WordsPerTransfer * sizeof(uint32_t))) {
        uart_puts("global RAM initialization remained enabled\n");
        return 1;
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

    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + MacroScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = macroExpected(transfer, word);
        }
    }
    if (!globalDMAMacroBegin(
            UINT64_C(3), MacroTransferCount, MacroInstructionBound)) {
        uart_puts("global DMA write macro begin failed\n");
        return 1;
    }
#if defined(MITTENS_SCRATCHPAD_DMA_INVALID_MACRO)
    // A certified macro may contain only its recorded submit/wait sequence.
    // This timed scratchpad access must be rejected by SST rather than being
    // silently omitted from the modeled event stream.
    auto* invalidMacroAccess = reinterpret_cast<volatile uint32_t*>(
        ScratchpadBase + MacroScratchpadOffset);
    *invalidMacroAccess = *invalidMacroAccess;
#endif
    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        if (!globalDMASubmit(
                MacroGlobalOffset + TransferStride * transfer,
                MacroScratchpadOffset + TransferStride * transfer,
                WordsPerTransfer * sizeof(uint32_t),
                UINT32_C(0x200) + transfer,
                UINT64_C(3),
                transfer,
                ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
            uart_puts("global DMA write macro submit failed\n");
            return 1;
        }
    }
    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        if (!globalDMAWait(UINT64_C(3), UINT32_C(0x200) + transfer)) {
            uart_puts("global DMA write macro wait failed\n");
            return 1;
        }
    }
    if (!globalDMAMacroEnd(UINT64_C(3), MacroTransferCount)) {
        uart_puts("global DMA write macro end failed\n");
        return 1;
    }

    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        auto* scratchpad = reinterpret_cast<uint32_t*>(
            ScratchpadBase + MacroScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            scratchpad[word] = 0;
        }
    }
    if (!globalDMAMacroBegin(
            UINT64_C(4), MacroTransferCount, MacroInstructionBound)) {
        uart_puts("global DMA read macro begin failed\n");
        return 1;
    }
    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        if (!globalDMASubmit(
                MacroGlobalOffset + TransferStride * transfer,
                MacroScratchpadOffset + TransferStride * transfer,
                WordsPerTransfer * sizeof(uint32_t),
                UINT32_C(0x300),
                UINT64_C(4),
                transfer,
                ScratchpadDMADirection::GlobalRAMToScratchpad)) {
            uart_puts("global DMA read macro submit failed\n");
            return 1;
        }
        // The production scalar descriptor window reuses its one hardware
        // token after every completed wait.  Keep this regression inside one
        // event tape so the macro and SST lifecycle must support that legal
        // submit/wait/submit/wait sequence.
        if (!globalDMAWait(UINT64_C(4), UINT32_C(0x300))) {
            uart_puts("global DMA read macro wait failed\n");
            return 1;
        }
    }
    if (!globalDMAMacroEnd(UINT64_C(4), MacroTransferCount)) {
        uart_puts("global DMA read macro end failed\n");
        return 1;
    }
    for (uint32_t transfer = 0; transfer < MacroTransferCount; ++transfer) {
        const auto* scratchpad = reinterpret_cast<const uint32_t*>(
            ScratchpadBase + MacroScratchpadOffset +
            TransferStride * transfer);
        for (uint32_t word = 0; word < WordsPerTransfer; ++word) {
            if (scratchpad[word] != macroExpected(transfer, word)) {
                uart_puts("global DMA macro data mismatch\n");
                return 1;
            }
        }
    }
    uart_puts("global DMA test: PASS\n");
    return 0;
}
