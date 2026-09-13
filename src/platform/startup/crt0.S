    .section .text.start,"ax",@progbits
    .align 2
    .globl _start
    .type _start, @function

_start:
    /* Initialize the RISC-V global pointer without linker relaxation. */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* The linker places the stack in the tile's private memory. */
    la sp, __stack_top
    andi sp, sp, -16

    /*
     * Enable floating-point and vector state for the lp64d+V Golem target.
     * FS occupies mstatus[14:13] and VS occupies mstatus[10:9].
     */
    li t0, ((3 << 13) | (3 << 9))
    csrs mstatus, t0
    csrwi fcsr, 0

    /* QEMU loads .data from the ELF; startup only needs to clear .bss. */
    la t0, __bss_start
    la t1, __bss_end
.Lclear_bss:
    bgeu t0, t1, .Lbss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j .Lclear_bss
.Lbss_done:

    call tile_main
    /* tile_main's return value is already in a0. */
    call platform_exit

.Lhalt:
    wfi
    j .Lhalt

    .size _start, . - _start
