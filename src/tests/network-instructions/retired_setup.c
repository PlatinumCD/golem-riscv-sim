#include <stdint.h>
#include "layout.h"
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

__attribute__((naked,aligned(4))) static void handler(void) {
    __asm__ volatile("csrr t0,mcause\n"
                     "li t1," STRINGIFY(REPORT_ADDRESS) "+24\n"
                     "sd t0,0(t1)\n"
                     "csrr t0,mepc\n"
                     "addi t0,t0,4\n"
                     "csrw mepc,t0\n"
                     "mret");
}
int main(void) {
    *SPM_LAST_WORD=0x53504d00+TILE;
    __asm__ volatile("csrw mtvec,%0" :: "r"(handler) : "memory");
    // The removed CUSTOM_1 funct3=5 encoding must trap, not configure an NIU.
    __asm__ volatile(".word 0x0000502b" ::: "t0","t1","memory");
    if (REPORT[3]!=2) return 97;
    REPORT[3]=0;
    // Even a zero value in a nonzero register is not a reserved-zero encoding.
    __asm__ volatile("li a0,0; .insn r 0x2b,1,0,a1,a0,zero" ::: "a0","a1","t0","t1","memory");
    if (REPORT[3]!=2) return 98;
    REPORT[3]=0;
    __asm__ volatile("li a0,0; .insn r 0x2b,1,1,a1,zero,a0" ::: "a0","a1","t0","t1","memory");
    if (REPORT[3]!=2) return 99;
    REPORT[0]=UINT64_C(0x4e45544f4b);
    return 0;
}
