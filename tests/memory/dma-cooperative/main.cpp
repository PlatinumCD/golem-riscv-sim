#include "platform.h"
#include "mesh-nic.h"
#include "scratchpad-dma.h"
using namespace golem::platform;
#ifndef MESH_ROUNDS
#define MESH_ROUNDS 64
#endif
#ifndef POLL_INTERVAL
#define POLL_INTERVAL (COOPERATIVE == 1 ? 1 : 16)
#endif
#ifndef WORK_ITERATIONS
#define WORK_ITERATIONS 0
#endif
#ifndef DETAILED_TRACE
#define DETAILED_TRACE 0
#endif
constexpr unsigned Bytes = 65536, Rounds = MESH_ROUNDS, Offset = 4096;
constexpr unsigned Execution = 901;
static void number(unsigned n) {
    if (n >= 10) number(n / 10);
    uart_putc('0' + n % 10);
}
static int failure(unsigned line) { uart_puts("DMA_COOPERATIVE_FAIL line="); number(line); uart_putc('\n'); return 1; }
#define fail() failure(__LINE__)
extern "C" int tile_main() {
    if (TILE == 1) {
        for (unsigned i = 0; i < Rounds; ++i) {
            mesh_nic::send(0, i + 17);
            unsigned value;
            while (!mesh_nic::try_receive(&value)) {}
            if (value != i + 17) return fail();
        }
        uart_puts("DMA_COOPERATIVE_PEER_PASS\n");
        return 0;
    }
    if (globalDMAQuery(Execution, 0) != GlobalDMACompletion::Error) return fail();
    if (globalDMAAcknowledge(Execution, 0)) return fail();
    unsigned long x0=1,x1=2,x2=3,x3=4,x4=5,x5=6,x6=7,x7=8;
    asm volatile("" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
                      "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) :: "memory");
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, 1, Execution);
    if (!globalDMASubmit(0, Offset, Bytes, 0, Execution, 0,
                         ScratchpadDMADirection::GlobalRAMToScratchpad)) return fail();
    bool done = false;
    unsigned replies = 0, polls = 0, pending = 0, iterations = 0;
    if (!COOPERATIVE) {
        if (!globalDMAWait(Execution, 0)) return fail();
        done = true;
    }
    if constexpr (WORK_ITERATIONS > 0) {
        // Exactly eight useful register adds per iteration; two loop-control
        // instructions. No data loads/stores in this assembly region.
        asm volatile(
            "li t0, %[count]\n.global dma_diagnostic_compute_begin\ndma_diagnostic_compute_begin:\n1:\n"
            "addi %0,%0,1\naddi %1,%1,1\naddi %2,%2,1\naddi %3,%3,1\n"
            "addi %4,%4,1\naddi %5,%5,1\naddi %6,%6,1\naddi %7,%7,1\n"
            "addi t0,t0,-1\nbnez t0,1b\n.global dma_diagnostic_compute_end\ndma_diagnostic_compute_end:\n"
            : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
              "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)
            : [count] "i"(WORK_ITERATIONS) : "t0");
    }
    while (!done || replies != Rounds) {
        if (!done && (COOPERATIVE == 3 || (iterations++ % POLL_INTERVAL) == 0)) {
            ++polls;
            const auto status = COOPERATIVE == 3 ? globalDMAWaitForEvent(Execution, 0) :
                                                  globalDMAQuery(Execution, 0);
            if (status == GlobalDMACompletion::Error) return fail();
            if (status == GlobalDMACompletion::Complete) {
                // Complete is retained and bytes must already be visible.
                if (globalDMAQuery(Execution, 0) != status) return fail();
                if (*reinterpret_cast<volatile unsigned*>(ScratchpadBase + Offset) != 0x03020100)
                    return fail();
                if (!globalDMAAcknowledge(Execution, 0)) return fail();
                done = true;
            } else ++pending;
        }
        unsigned value;
        if (replies != Rounds && mesh_nic::try_receive(&value)) {
            if constexpr (DETAILED_TRACE)
                mesh_nic::trace_task(mesh_nic::kTaskTraceStart, 1000 + replies, Execution);
            if (value != replies + 17) return fail();
            mesh_nic::send(1, value);
            if (replies++ == 0)
                mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, 2, Execution);
        } else if (COOPERATIVE == 3 && done && replies != Rounds) {
            mesh_nic::wait_for_receive();
        }
    }
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, 1, Execution);
    if (x0+x1+x2+x3+x4+x5+x6+x7 != 36UL+8UL*WORK_ITERATIONS) return fail();
    for (unsigned i = 0; i < Bytes; ++i)
        if (*reinterpret_cast<volatile unsigned char*>(ScratchpadBase + Offset + i) != (i & 255))
            return fail();
    if (globalDMAQuery(Execution, 0) != GlobalDMACompletion::Error) return fail();
    // Same token, different invocation must not observe the old completion.
    if (!globalDMASubmit(0, Offset, 32, 0, Execution + 1, 0,
                         ScratchpadDMADirection::GlobalRAMToScratchpad)) return fail();
    if (globalDMAQuery(Execution, 0) != GlobalDMACompletion::Error) return fail();
    if (!globalDMAWait(Execution + 1, 0)) return fail();
    // All eight slots include retained completions. A ninth submit cannot
    // overwrite one, and querying a completion must not release its slot.
    for (unsigned token = 0; token < 8; ++token)
        if (!globalDMASubmit(0, Offset + token * 32, 32, token, Execution + 2, token,
                             ScratchpadDMADirection::GlobalRAMToScratchpad)) return fail();
    if (globalDMASubmit(0, Offset + 256, 32, 8, Execution + 2, 8,
                        ScratchpadDMADirection::GlobalRAMToScratchpad)) return fail();
    for (unsigned token = 0; token < 8; ++token) {
        GlobalDMACompletion status;
        do { status = globalDMAQuery(Execution + 2, token); }
        while (status == GlobalDMACompletion::Pending);
        if (status != GlobalDMACompletion::Complete) return fail();
    }
    if (globalDMASubmit(0, Offset + 256, 32, 8, Execution + 2, 8,
                        ScratchpadDMADirection::GlobalRAMToScratchpad)) return fail();
    for (unsigned token = 0; token < 8; ++token)
        if (!globalDMAAcknowledge(Execution + 2, token)) return fail();
    // Outgoing data becomes durable before Complete; repeated queries do not
    // copy again after the caller reuses its source storage.
    if (!globalDMASubmit(131072, Offset, 32, 0, Execution + 3, 0,
                         ScratchpadDMADirection::ScratchpadToGlobalRAM)) return fail();
    GlobalDMACompletion status;
    do { status = globalDMAQuery(Execution + 3, 0); }
    while (status == GlobalDMACompletion::Pending);
    if (status != GlobalDMACompletion::Complete) return fail();
    *reinterpret_cast<volatile unsigned*>(ScratchpadBase + Offset) = 0;
    if (globalDMAQuery(Execution + 3, 0) != GlobalDMACompletion::Complete) return fail();
    if (!globalDMAAcknowledge(Execution + 3, 0)) return fail();
    if (!globalDMASubmit(131072, Offset, 32, 0, Execution + 4, 0,
                         ScratchpadDMADirection::GlobalRAMToScratchpad) ||
        !globalDMAWait(Execution + 4, 0)) return fail();
    if (*reinterpret_cast<volatile unsigned*>(ScratchpadBase + Offset) != 0x03020100) return fail();
    uart_puts("DMA_COOPERATIVE_PASS polls="); number(polls);
    uart_puts(" pending="); number(pending); uart_putc('\n');
    return 0;
}
