.section .text.start,"ax",@progbits
.globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __stack_top
    la t0, guest_trap
    csrw mtvec,t0
    # Enable floating point and RVV before touching any vector state.
    li t0,0x6600
    csrs mstatus,t0
    call main
.globl guest_exit
guest_exit:
    li t0,0x100000
    li t1,0x5555
    beqz a0,1f
    slli a0,a0,16
    li t1,0x3333
    or t1,t1,a0
1:  sw t1,0(t0)
2:  j 2b

.balign 4
.globl guest_trap
guest_trap:
    # Preserve useful failure evidence even for unexpected synchronous traps.
    li t0,0x90101000
    li t1,-1
    sw t1,8(t0)
    li t1,30
    sw t1,40(t0)
    csrr t1,mcause
    sw t1,44(t0)
    csrr t1,mepc
    sw t1,48(t0)
    fence rw,rw
    li a0,30
    j guest_exit
