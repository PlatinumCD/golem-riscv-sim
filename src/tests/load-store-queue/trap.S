.text
.balign 4
.globl trap_handler
trap_handler:
    addi sp, sp, -32
    sd t0, 0(sp)
    sd t1, 8(sp)
#if MODE == 7
    /* Capture the loaded register and the older store at trap entry. The host
     * also checks retirement before the FIRST handler instruction fetch, so
     * these observing instructions cannot conceal a missing entry drain. */
    li t0, 0x90150000
    vse32.v v8, (t0)
    li t0, 0x90124000
    lw t1, 0(t0)
    li t0, 0x90150080
    sw t1, 0(t0)
#endif
    la t0, trap_record
    csrr t1, mcause
    sd t1, 0(t0)
    csrr t1, mtval
    sd t1, 8(t0)
    csrr t1, vstart
    sd t1, 16(t0)
    ld t1, 24(t0)
    addi t1, t1, 1
    sd t1, 24(t0)
#if MODE == 7
    la t0, fetch_fault_resume
#else
    csrr t0, mepc
    addi t0, t0, 4
#endif
    csrw mepc, t0
    csrwi vstart, 0
    ld t0, 0(sp)
    ld t1, 8(sp)
    addi sp, sp, 32
    mret
