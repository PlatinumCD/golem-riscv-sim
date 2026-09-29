.section .text
.align 2
.global trap_handler
.type trap_handler, @function
trap_handler:
    addi sp, sp, -32
    sd t0, 0(sp)
    sd t1, 8(sp)
    sd t2, 16(sp)
    sd t3, 24(sp)
    la t0, traps
    lw t1, 0(t0)
    li t2, 3
    bgeu t1, t2, 1f
    slli t2, t1, 3
    la t3, trap_causes
    add t3, t3, t2
    csrr t1, mcause
    sd t1, 0(t3)
    la t3, trap_values
    add t3, t3, t2
    csrr t1, mtval
    sd t1, 0(t3)
    la t3, trap_starts
    add t3, t3, t2
    csrr t1, vstart
    sd t1, 0(t3)
    lw t1, 0(t0)
    addi t1, t1, 1
    sw t1, 0(t0)
    j 2f
1:  la t0, unexpected_traps
    li t1, 1
    sw t1, 0(t0)
2:  csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    csrwi vstart, 0
    ld t0, 0(sp)
    ld t1, 8(sp)
    ld t2, 16(sp)
    ld t3, 24(sp)
    addi sp, sp, 32
    mret
.size trap_handler, .-trap_handler
