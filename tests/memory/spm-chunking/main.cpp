#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"
#include <stdint.h>

#ifndef DATA_BYTES
#define DATA_BYTES 65548
#endif
#ifndef CHUNK_BYTES
#define CHUNK_BYTES 4096
#endif
static_assert(DATA_BYTES > 16384 && DATA_BYTES % 4 == 0);
static_assert(CHUNK_BYTES > 0 && CHUNK_BYTES <= 4096 && CHUNK_BYTES % 4 == 0);

namespace {
constexpr uint32_t Poison = 0xdeadc0de;
constexpr uint32_t Guard = 0xcafebabe;
constexpr uint64_t Input = 0x100000, Output = 0x200000;
static_assert(Input + DATA_BYTES <= Output && Output + DATA_BYTES <= 0x400000);
struct alignas(32) Window {
    volatile uint32_t before[8];
    volatile uint32_t words[1024];
    volatile uint32_t after[8];
};
Window window;
uint32_t input(uint32_t index) { return index ^ 0x13570000U; }
uint32_t transform(uint32_t value) { return (value + 17U) ^ 0xa5a50000U; }
[[noreturn]] void fail() { uart_puts("SPM_CHUNKING_FAIL\n"); platform_exit(1); }
void poison() { for (auto& word : window.words) word = Poison; }
void checkBounds(uint32_t used) {
    for (uint32_t i=0; i<8; ++i)
        if (window.before[i]!=Guard || window.after[i]!=Guard) fail();
    for (uint32_t i=used; i<1024; ++i)
        if (window.words[i]!=Poison) fail();
}
void transfer(uint64_t address, uint32_t bytes, uint32_t phase, uint32_t chunk,
              golem::platform::ScratchpadDMADirection direction) {
    using namespace golem::platform;
    const auto offset = reinterpret_cast<uintptr_t>(window.words)-ScratchpadBase;
    // Reuse one descriptor only after its previous transfer has completed.
    if (!globalDMASubmit(address, offset, bytes, 1, phase, chunk, direction) ||
        !globalDMAWait(phase, 1)) fail();
}
}

extern "C" int tile_main() {
    using namespace golem::platform;
    for (uint32_t i=0; i<8; ++i) window.before[i]=window.after[i]=Guard;
    // Phase 1: construct the input in shared RAM through the same bounded
    // window. The dataset is never a large array in the guest ELF or SPM.
    for (uint32_t phase=1; phase<=3; ++phase) {
        mesh_nic::trace_task(mesh_nic::kTaskTraceStart, phase, 0);
        uint32_t chunk=0;
        for (uint32_t offset=0; offset<DATA_BYTES; offset+=CHUNK_BYTES, ++chunk) {
            const uint32_t bytes = DATA_BYTES-offset < CHUNK_BYTES ? DATA_BYTES-offset : CHUNK_BYTES;
            const uint32_t words=bytes/4;
            poison();
            if (phase==1) {
                for (uint32_t i=0; i<words; ++i) window.words[i]=input(offset/4+i);
                transfer(Input+offset, bytes, phase, chunk, ScratchpadDMADirection::ScratchpadToGlobalRAM);
            } else if (phase==2) {
                transfer(Input+offset, bytes, phase, chunk, ScratchpadDMADirection::GlobalRAMToScratchpad);
                for (uint32_t i=0; i<words; ++i) {
                    if (window.words[i]!=input(offset/4+i)) fail();
                    window.words[i]=transform(window.words[i]);
                }
                transfer(Output+offset, bytes, phase, chunk, ScratchpadDMADirection::ScratchpadToGlobalRAM);
            } else {
                transfer(Output+offset, bytes, phase, chunk, ScratchpadDMADirection::GlobalRAMToScratchpad);
                for (uint32_t i=0; i<words; ++i)
                    if (window.words[i]!=transform(input(offset/4+i))) fail();
            }
            checkBounds(words);
        }
        mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, phase, 0);
    }
    uart_puts("SPM_CHUNKING_PASS\n");
    return 0;
}
