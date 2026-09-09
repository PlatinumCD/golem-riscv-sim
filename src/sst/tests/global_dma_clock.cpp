#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef GLOBAL_DMA_CLOCK_BATCH_WAIT
#define GLOBAL_DMA_CLOCK_BATCH_WAIT 0
#endif

namespace {
constexpr uint32_t Count = 2;
constexpr uint32_t Bytes = 128;
constexpr uint64_t Execution = 501;
constexpr uint64_t GlobalOffset = 4096;
constexpr uint64_t SourceOffset = 4096;
constexpr uint64_t CheckOffset = 8192;

uint32_t expected(uint32_t transfer, uint32_t word) {
    return UINT32_C(0x7bd12300) ^ (transfer << 16U) ^ word;
}

int fail(const char* message) {
    uart_puts(message);
    uart_putc('\n');
    return 1;
}
}

extern "C" int tile_main() {
    using namespace golem::platform;
    // Initialize before the aggregate marker: no CPU SPM access competes with
    // either measured source-SPM DMA reservation.
    for (uint32_t transfer = 0; transfer < Count; ++transfer) {
        auto* source = reinterpret_cast<volatile uint32_t*>(
            ScratchpadBase + SourceOffset + transfer * Bytes);
        for (uint32_t word = 0; word < Bytes / sizeof(uint32_t); ++word) {
            source[word] = expected(transfer, word);
        }
    }
    mesh_nic::complete_memory_initialization();
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, 51, Execution);
    for (uint32_t transfer = 0; transfer < Count; ++transfer) {
        if (!globalDMASubmit(
                GlobalOffset + transfer * Bytes,
                SourceOffset + transfer * Bytes, Bytes, transfer, Execution,
                transfer, ScratchpadDMADirection::ScratchpadToGlobalRAM)) {
            return fail("GLOBAL_DMA_CLOCK_SUBMIT_FAIL");
        }
    }
#if GLOBAL_DMA_CLOCK_BATCH_WAIT
    if (!globalDMAWaitBatch(Execution, 0, Count)) {
        return fail("GLOBAL_DMA_CLOCK_BATCH_WAIT_FAIL");
    }
#else
    for (uint32_t transfer = 0; transfer < Count; ++transfer) {
        if (!globalDMAWait(Execution, transfer)) {
            return fail("GLOBAL_DMA_CLOCK_WAIT_FAIL");
        }
    }
#endif
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, 51, Execution);

    // Verify actual copied data outside the measured region, using a distinct
    // execution ID so these transfers cannot enter the timing oracle.
    for (uint32_t transfer = 0; transfer < Count; ++transfer) {
        if (!globalDMASubmit(
                GlobalOffset + transfer * Bytes, CheckOffset, Bytes,
                transfer, Execution + 1, transfer,
                ScratchpadDMADirection::GlobalRAMToScratchpad) ||
            !globalDMAWait(Execution + 1, transfer)) {
            return fail("GLOBAL_DMA_CLOCK_READBACK_FAIL");
        }
        const auto* data = reinterpret_cast<const volatile uint32_t*>(
            ScratchpadBase + CheckOffset);
        for (uint32_t word = 0; word < Bytes / sizeof(uint32_t); ++word) {
            if (data[word] != expected(transfer, word)) {
                return fail("GLOBAL_DMA_CLOCK_DATA_FAIL");
            }
        }
    }
    uart_puts("GLOBAL_DMA_CLOCK_PAYLOAD_PASS\n");
    return 0;
}
