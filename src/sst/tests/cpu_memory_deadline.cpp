#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"
#include <stdint.h>

#ifndef DEADLINE_SPM
#define DEADLINE_SPM 0
#endif
extern "C" int tile_main() {
    using namespace golem::platform;
    // Explicit disjoint locations keep compiler stack traffic out of the oracle.
    auto* ram = reinterpret_cast<volatile uint32_t*>(UINT64_C(0x80800000));
    auto* spm = reinterpret_cast<volatile uint32_t*>(ScratchpadBase + 4096);
    *spm = 0x12345;
    mesh_nic::complete_memory_initialization();
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, 61, 601);
    uint32_t value;
#if DEADLINE_SPM
    // Both SPM loads must keep their own service deadline. The independent
    // buffered write acknowledges during the first load's service interval.
    asm volatile("sw zero, 0(%1)\n\tlw %0, 0(%2)\n\tlw %0, 0(%2)"
                 : "=&r"(value) : "r"(ram), "r"(spm) : "memory");
#else
    // The fence has been captured but is not eligible for execution until the
    // 4096 register-only instructions have consumed CPU-domain cycles.
    asm volatile("sw zero, 0(%1)\n\tli %0, 0\n\t"
                 ".rept 4096\n\taddi %0, %0, 1\n\t.endr\n\tfence rw, rw"
                 : "=&r"(value) : "r"(ram) : "memory");
#endif
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, 61, 601);
    if (value != (DEADLINE_SPM ? 0x12345U : 4096U)) return 1;
    uart_puts("CPU_MEMORY_DEADLINE_PAYLOAD_PASS\n");
    return 0;
}
