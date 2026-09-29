#ifndef VLEN_BITS
#define VLEN_BITS 256
#endif
#define LANES (VLEN_BITS / 32)
#define VECTOR_BYTES (VLEN_BITS / 8)
#define N (LANES * 8)
#define GROUP_BYTES (VECTOR_BYTES * 8)
#define OUTPUT_BASE 0x90110000
#define TRAP_COUNT 0x901e0000
#define TRAP_OUTPUT 0x90150000

.text
.balign 64
.global main
.type main,@function
main:
    addi sp,sp,-64
    sd ra,56(sp)
    sd s0,48(sp)
    sd s1,40(sp)
    sd s2,32(sp)
    sd s3,24(sp)
    sd s4,16(sp)
    la t0,trap_handler
    csrw mtvec,t0
    la s0,input_data
    la s1,noise_data
    la s2,weights
    li s3,OUTPUT_BASE

.macro config mode,count
    li t2,\count
    vsetvli zero,t2,e32,\mode,tu,mu
.endm
.macro mark phase,start
    li a0,\phase
    li a1,\start
    call benchmark_marker
.endm
.macro outptr slot
    li a3,(OUTPUT_BASE+4096*\slot)
.endm
.macro snapshot reg,slot
    outptr \slot
    vs8r.v v\reg,(a3)
.endm
.macro snapshot1 reg,slot
    outptr \slot
    vs1r.v v\reg,(a3)
.endm
.macro mvm_run
    mvm t4,zero,zero
    bnez t4,failed
.endm
.macro save_result slot
    config m8,N
    mvm.vs v8,zero,zero
    snapshot 8,\slot
.endm
.macro fresh_input
    config m8,N
    vle32.v v8,(s0)
    mvm.vl v8,zero,zero
.endm

    /* First full matrix chunk: dependent source and younger independent group. */
    config m8,N
    mark 1,1
.balign 64
.global issue_p1_related
issue_p1_related:
    vle32.v v8,(s2)
.global issue_p1_unrelated
issue_p1_unrelated:
    vle32.v v16,(s1)
.global target_p1
 target_p1:
    mvm.vset v8,zero,zero
    snapshot 16,1
    mark 1,0
    /* Finish real identity-matrix programming outside the directed phases. */
    li t0,N
    li t1,(N*N)
    addi a2,s2,GROUP_BYTES
.Lprogram:
    vle32.v v8,(a2)
    mvm.vset v8,zero,t0
    addi a2,a2,GROUP_BYTES
    addi t0,t0,N
    bltu t0,t1,.Lprogram
    fresh_input
    mvm_run
    save_result 0

    /* Ready analog source must not wait for an unrelated loaded group. */
    vle32.v v8,(s0)
    mark 2,1
.balign 64
.global issue_p2_unrelated
issue_p2_unrelated:
    vle32.v v16,(s1)
.global target_p2
 target_p2:
    mvm.vl v8,zero,zero
    snapshot 16,2
    mark 2,0
    mvm_run
    save_result 3

    /* The true source dependency is retained, independent younger work is not. */
    vle32.v v8,(s1)
    mark 3,1
.balign 64
.global issue_p3_related
issue_p3_related:
    vle32.v v8,(s0)
.global issue_p3_unrelated
issue_p3_unrelated:
    vle32.v v16,(s1)
.global target_p3
 target_p3:
    mvm.vl v8,zero,zero
    snapshot 16,4
    mark 3,0
    mvm_run
    save_result 5

    /* Destination WAW: an older load must not clobber the later array output. */
    mark 4,1
.balign 64
.global issue_p4_related
issue_p4_related:
    vle32.v v8,(s1)
.global issue_p4_unrelated
issue_p4_unrelated:
    vle32.v v16,(s1)
.global target_p4
 target_p4:
    mvm.vs v8,zero,zero
    snapshot 8,6
    snapshot 16,7
    mark 4,0

    /* Stores capture their source payload, so the register may be overwritten. */
    vle32.v v8,(s1)
    outptr 8
    mark 5,1
.balign 64
.global issue_p5_store
issue_p5_store:
    vse32.v v8,(a3)
.global target_p5
 target_p5:
    mvm.vs v8,zero,zero
    snapshot 8,9
    mark 5,0

    /* Pending load to the LAST physical register of an m8 source. */
    fresh_input
    config m1,LANES
    addi a2,s1,(3*VECTOR_BYTES)
    mark 6,1
.balign 64
.global issue_p6_related
issue_p6_related:
    vle32.v v15,(a2)
.global issue_p6_unrelated
issue_p6_unrelated:
    vle32.v v24,(s1)
    config m8,N
.global target_p6
 target_p6:
    mvm.vl v8,zero,zero
    snapshot1 24,10
    mark 6,0
    mvm_run
    save_result 11

    /* Fractional-LMUL write preserves the physical register's inactive tail. */
    config m1,LANES
    mark 7,1
.balign 64
.global issue_p7_related
issue_p7_related:
    vle32.v v8,(s1)
.global issue_p7_unrelated
issue_p7_unrelated:
    vle32.v v16,(s1)
    config mf2,(LANES/2)
.global target_p7
 target_p7:
    mvm.vs v8,zero,zero
    snapshot1 8,12
    snapshot1 16,13
    mark 7,0

    /* Short-VL m8 keeps tails: the full-group dependency mask is conservative. */
    config m8,N
    vle32.v v8,(s1)
    config m1,LANES
    addi a2,s1,(3*VECTOR_BYTES)
    mark 8,1
.balign 64
.global issue_p8_related
issue_p8_related:
    vle32.v v15,(a2)
    config m8,1
.global target_p8
 target_p8:
    mvm.vs v8,zero,zero
    snapshot 8,14
    mark 8,0

    /* Scalar compute has no vector register dependency. */
    fresh_input
    mark 9,1
.balign 64
.global issue_p9_unrelated
issue_p9_unrelated:
    vle32.v v16,(s1)
.global target_p9
 target_p9:
    mvm t4,zero,zero
    bnez t4,failed
    snapshot 16,15
    mark 9,0
    save_result 16

    /* Legal encoding, dynamic bad array: older memory must retire on trap. */
    config m1,LANES
    vle32.v v8,(s0)
    vle32.v v24,(s1)
    li a2,-1
    outptr 17
    mark 10,1
.balign 64
.global issue_p10_load
issue_p10_load:
    vle32.v v16,(s1)
.global issue_p10_store
issue_p10_store:
    vse32.v v24,(a3)
.global target_p10
 target_p10:
    mvm.vl v8,a2,zero
    snapshot1 8,19
    mark 10,0

    /* Legal output encoding, dynamic invalid offset must preserve destination. */
    vle32.v v8,(s1)
    li a2,N
    outptr 17
    mark 11,1
.balign 64
.global issue_p11_load
issue_p11_load:
    vle32.v v16,(s1)
.global issue_p11_store
issue_p11_store:
    vse32.v v24,(a3)
.global target_p11
 target_p11:
    mvm.vs v8,zero,a2
    snapshot1 8,20
    mark 11,0

    /* Invalid register alignment is rejected before an analog command. */
    config m2,LANES
    outptr 17
    mark 12,1
.balign 64
.global issue_p12_load
issue_p12_load:
    vle32.v v16,(s1)
.global issue_p12_store
issue_p12_store:
    vse32.v v24,(a3)
.global target_p12
 target_p12:
    mvm.vl v9,zero,zero
    mark 12,0

    li t0,TRAP_COUNT
    lw t1,0(t0)
    li t2,3
    bne t1,t2,failed
    li a0,0
    j finish
failed:
    li a0,1
finish:
    ld s4,16(sp)
    ld s3,24(sp)
    ld s2,32(sp)
    ld s1,40(sp)
    ld s0,48(sp)
    ld ra,56(sp)
    addi sp,sp,64
    ret
.size main,.-main

.balign 64
.global trap_handler
.type trap_handler,@function
trap_handler:
    /* First lookup is the timing oracle: all older loads/stores must already
     * have committed, before even these handler stack accesses. */
    addi sp,sp,-48
    sd t0,0(sp)
    sd t1,8(sp)
    sd t2,16(sp)
    sd t3,24(sp)
    sd t4,32(sp)
    li t0,TRAP_COUNT
    lw t1,0(t0)
    li t2,TRAP_OUTPUT
    slli t3,t1,12
    add t2,t2,t3
    vs1r.v v16,(t2)
    addi t2,t2,128
    csrr t3,mcause
    sd t3,0(t2)
    csrr t3,mepc
    sd t3,8(t2)
    li t4,(OUTPUT_BASE+17*4096)
    lw t4,0(t4)
    sw t4,16(t2)
    addi t1,t1,1
    sw t1,0(t0)
    addi t3,t3,4
    csrw mepc,t3
    csrwi vstart,0
    ld t0,0(sp)
    ld t1,8(sp)
    ld t2,16(sp)
    ld t3,24(sp)
    ld t4,32(sp)
    addi sp,sp,48
    mret
.size trap_handler,.-trap_handler
