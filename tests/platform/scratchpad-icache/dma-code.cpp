#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

namespace {
// The boot loader stages these bytes in shared RAM as part of this ELF.
alignas(32) const uint32_t image[] = {0x00700513, 0x00008067}; // return 7
alignas(64) uint32_t executable[16];
}

extern "C" int tile_main()
{
    using namespace golem::platform;
    const auto source = reinterpret_cast<uintptr_t>(image) - ScratchpadBase;
    const auto target = reinterpret_cast<uintptr_t>(executable) - ScratchpadBase;
    // This fixture is tile zero, whose shared-RAM boot slot starts at zero.
    if (!globalDMASubmit(source, target, sizeof(image), 1, 1, 0,
                         ScratchpadDMADirection::GlobalRAMToScratchpad) ||
        !globalDMAWait(1, 1))
        return 1;
    asm volatile("fence.i" ::: "memory");
    const auto kernel = reinterpret_cast<int (*)()>(executable);
    if (kernel() != 7)
        return 2;
    uart_puts("SCRATCHPAD_ICACHE_PASS\n");
    return 0;
}
