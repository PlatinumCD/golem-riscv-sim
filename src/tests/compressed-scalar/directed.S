#define N 64
#define OUTPUT 0x90110000
#define SCALAR 0x901d0000
#define TRAP 0x901e0000
#ifndef FAULT_BYTES
#define FAULT_BYTES 2
#endif

.text
.option norvc
.option norelax
.option push
.option rvc
.balign 64
.option pop
.global main
main:
    addi sp,sp,-80
    sd ra,72(sp)
    sd s0,64(sp)
    sd s1,56(sp)
    sd s2,48(sp)
    sd a4,40(sp)
    sd a5,32(sp)
    la s0,input_data
    la s1,noise_data
    la s2,weights
    la t0,trap_handler
    csrw mtvec,t0
    li t2,N
    vsetvli zero,t2,e32,m8,tu,mu

.macro mark phase,start
    li a0,\phase
    li a1,\start
    call benchmark_marker
.endm
.macro addword lhs,rhs
    .option push
    .option rvc
    c.addw \lhs,\rhs
    .option pop
.endm
.macro save reg,slot
    li a3,(OUTPUT+4096*\slot)
    vs8r.v v\reg,(a3)
.endm
.macro scalar slot
    li a3,(SCALAR+8*\slot)
    sd a4,0(a3)
.endm

    /* A word overflow sign-extends; a vector RAW still waits for the load. */
    li a4,0x7fffffff
    li a5,1
    mark 1,1
.option push
.option rvc
.balign 64
.option pop
.global memory_p1
memory_p1:
    vle32.v v8,(s0)
.global scalar_p1
scalar_p1:
    addword a4,a5
.global dependent_p1
dependent_p1:
    vadd.vi v16,v8,1
    save 16,0
    scalar 0
    mark 1,0

    /* Captured stores survive rd=rs2; overlapping reads retain memory order. */
    vle32.v v8,(s1)
    li a3,(OUTPUT+4096)
    li a4,0x80000001
    mark 2,1
.option push
.option rvc
.balign 64
.option pop
.global memory_p2
memory_p2:
    vse32.v v8,(a3)
.global scalar_p2
scalar_p2:
    addword a4,a4
.global dependent_p2
dependent_p2:
    vle32.v v16,(a3)
    save 16,2
    scalar 1
    mark 2,0

    /* An independent scalar does not release a still-pinned array source. */
    vle32.v v8,(s2)
    li a4,0x1234567800000000
    li a5,-1
    mark 3,1
.option push
.option rvc
.balign 64
.option pop
.global command_p3
command_p3:
    mvm.vset v8,zero,zero
.global scalar_p3
scalar_p3:
    addword a4,a5
.global dependent_p3
dependent_p3:
    vmv.v.i v8,0
    save 8,3
    scalar 2
    mark 3,0

    /* Complete the identity matrix and prove the first chunk was captured. */
    li t0,N
    li t1,(N*N)
    addi a2,s2,(N*4)
.Lprogram:
    vle32.v v8,(a2)
    mvm.vset v8,zero,t0
    addi a2,a2,(N*4)
    addi t0,t0,N
    bltu t0,t1,.Lprogram
    vle32.v v8,(s0)
    mvm.vl v8,zero,zero
    mvm t4,zero,zero
    bnez t4,failed

    /* A scalar may pass pending array output; its vector consumer may not. */
    li a4,-2147483648
    li a5,-1
    mark 4,1
.option push
.option rvc
.balign 64
.option pop
.global command_p4
command_p4:
    mvm.vs v8,zero,zero
.global scalar_p4
scalar_p4:
    addword a4,a5
.global dependent_p4
dependent_p4:
    vadd.vi v16,v8,1
    save 16,4
    scalar 3
    mark 4,0

    /* This compressed neighbor is absent from this QEMU decoder, even Zcb.
     * Trapping must drain both queues and publish all older memory first. */
    vle32.v v0,(s2)
    vle32.v v24,(s1)
    li a3,(OUTPUT+5*4096)
    mark 5,1
.option push
.option rvc
.balign 64
.option pop
.global command_p5
command_p5:
    mvm.vset v0,zero,zero
.global memory_p5_load
memory_p5_load:
    vle32.v v16,(s0)
.global memory_p5_store
memory_p5_store:
    vse32.v v24,(a3)
.global fault_p5
fault_p5:
#if FAULT_BYTES == 2
    .2byte 0x9c79
#else
    /* Reserved RV64 M word encoding: funct3=1 has no MULHW operation. */
    .4byte 0x02f7173b
#endif
    mark 5,0
#include "scalar-phases.h"
    li a0,0
    j finish
failed:
    li a0,1
finish:
    ld a5,32(sp)
    ld a4,40(sp)
    ld s2,48(sp)
    ld s1,56(sp)
    ld s0,64(sp)
    ld ra,72(sp)
    addi sp,sp,80
    ret

.option push
.option rvc
.balign 64
.option pop
.global trap_handler
trap_handler:
    addi sp,sp,-32
    sd t0,0(sp)
    sd t1,8(sp)
    li t0,TRAP
    lw t1,0(t0)
    addi t1,t1,1
    sw t1,0(t0)
    csrr t1,mcause
    sd t1,8(t0)
    csrr t1,mepc
    sd t1,16(t0)
    addi t1,t1,FAULT_BYTES
    csrw mepc,t1
    addi t0,t0,256
    vs8r.v v16,(t0)
    li t0,(OUTPUT+5*4096)
    lw t1,0(t0)
    li t0,(TRAP+24)
    sw t1,0(t0)
    ld t0,0(sp)
    ld t1,8(sp)
    addi sp,sp,32
    mret
