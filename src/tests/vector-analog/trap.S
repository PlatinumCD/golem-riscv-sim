.text
.balign 4
.globl trap_handler
trap_handler:
    addi sp, sp, -32
    sd t0, 0(sp)
    sd t1, 8(sp)
    csrr t0, mcause
    li t1, 2
    bne t0, t1, 1f
    la t0, traps
    j 2f
1:  la t0, unexpected_traps
2:  lw t1, 0(t0)
    addi t1, t1, 1
    sw t1, 0(t0)
    # vstart CSR is inaccessible with VS=Off. Only clear it when enabled.
    csrr t0, mstatus
    andi t0, t0, 0x600
    beqz t0, 3f
    csrwi vstart, 0
3:  csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    ld t0, 0(sp)
    ld t1, 8(sp)
    addi sp, sp, 32
    mret
