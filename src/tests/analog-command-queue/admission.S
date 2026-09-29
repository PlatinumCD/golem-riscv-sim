.text
.option norvc
.balign 64
.global main
main:
    addi sp,sp,-32
    sd ra,24(sp)
    la t0,trap_handler
    csrw mtvec,t0
    li t2,64
    vsetvli zero,t2,e32,m8,tu,mu
    la a2,weights
    vle32.v v0,(a2)
    la a2,noise_data
    vle32.v v8,(a2)
    li a4,1
    li a0,1
    li a1,1
    call benchmark_marker
.balign 64
.global command_program
command_program:
    mvm.vset v0,zero,zero
    .rept 12
    addi t3,t3,1
    .endr
.global target_fault
target_fault:
    /* Legal array and range, but no compute result exists. This cannot receive
     * a success guarantee: Accepted must remain blocking through Error. */
    mvm.vs v8,a4,zero
    li a3,0x90110000
    vs8r.v v8,(a3)
    li a0,1
    li a1,0
    call benchmark_marker
    li a0,0
    ld ra,24(sp)
    addi sp,sp,32
    ret
.balign 64
.global trap_handler
trap_handler:
    addi sp,sp,-24
    sd t0,0(sp)
    sd t1,8(sp)
    li t0,0x901e0000
    lw t1,0(t0)
    addi t1,t1,1
    sw t1,0(t0)
    csrr t1,mcause
    sd t1,8(t0)
    csrr t1,mepc
    sd t1,16(t0)
    addi t1,t1,4
    csrw mepc,t1
    ld t0,0(sp)
    ld t1,8(sp)
    addi sp,sp,24
    mret
