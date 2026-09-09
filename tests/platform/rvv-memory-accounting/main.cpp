#include "mesh-nic.h"
#include "platform.h"
#include <stdint.h>

#ifndef KERNEL
#error KERNEL must select a boundary regression
#endif

extern "C" int tile_main()
{
    constexpr uintptr_t spm = 0x90000000;
    for (unsigned i = 0; i < 8; ++i)
        reinterpret_cast<volatile uint32_t*>(spm)[i] = 1;
    mesh_nic::complete_memory_initialization();
    asm volatile("vsetivli zero, 8, e32, m1, ta, ma\n\tvmv.v.i v9, 0"
                 ::: "v9", "memory");
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, 1, 0);

    // Exactly 64 repetitions; setup operands are identical across grant sizes.
    // No guest loop or compiler-generated vector instructions in these bodies.
    asm volatile(
        "mv a0, %0\n\t"
        "li a1, 1\n\t"
        ".option push\n\t"
        ".option rvc\n\t"
        ".rept 64\n\t"
#if KERNEL == 0
        "vle32.v v8, (a0)\n\t"
        "vadd.vv v9, v9, v8\n\t"
#elif KERNEL == 1
        "fence\n\t"
        "vadd.vx v9, v9, a1\n\t"
        "vse32.v v9, (a0)\n\t"
#elif KERNEL == 2
        "c.lw a4, 0(a0)\n\t"
        "vadd.vx v9, v9, a4\n\t"
        "c.sw a4, 0(a0)\n\t"
#else
#error unknown KERNEL
#endif
        ".endr\n\t.option pop"
        : : "r"(spm) : "a0", "a1", "a4", "v8", "v9", "memory");
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, 1, 0);
    uint32_t value;
    asm volatile("vmv.x.s %0, v9" : "=r"(value) : : "v9");
    if (value != 64) {
        uart_puts("RVV_MEMORY_ACCOUNTING_FAIL\n");
        return 1;
    }
    uart_puts("RVV_MEMORY_ACCOUNTING_PASS\n");
    return 0;
}
