#ifndef VLEN_BITS
#define VLEN_BITS 256
#endif
#define LANES (VLEN_BITS / 32)
#define N (LANES * 8)
#define GROUP_BYTES (N * 4)
#define OUTPUT 0x90110000

.text
.option norvc
.balign 64
.global main
main:
    addi sp,sp,-64
    sd ra,56(sp)
    sd s0,48(sp)
    sd s1,40(sp)
    sd s2,32(sp)
    sd s3,24(sp)
    sd s4,16(sp)
    la s0,input_data
    la s1,noise_data
    la s2,weights
    la s3,weights_two
    li s4,1
    la t0,trap_handler
    csrw mtvec,t0

.macro cfg mode,count
    li t2,\count
    vsetvli zero,t2,e32,\mode,tu,mu
.endm
.macro mark phase,start
    li a0,\phase
    li a1,\start
    call benchmark_marker
.endm
.macro save reg,slot
    li a3,(OUTPUT+4096*\slot)
    vs8r.v v\reg,(a3)
.endm
.macro save1 reg,slot
    li a3,(OUTPUT+4096*\slot)
    vs1r.v v\reg,(a3)
.endm
.macro job array,source
    cfg m8,N
    vle32.v v8,(\source)
    mvm.vl v8,\array,zero
    mvm t4,\array,zero
    bnez t4,failed
.endm

    /* Source WAR releases after delivery, before the long programming delay. */
    cfg m8,N
    vle32.v v8,(s2)
    mark 1,1
.balign 64
.global command_p1
command_p1:
    mvm.vset v8,zero,zero
.global target_p1
target_p1:
    vmv.v.i v8,0
    save 8,0
    mark 1,0
    li t0,N
    li t1,(N*N)
    addi a2,s2,GROUP_BYTES
.Lprogram0:
    vle32.v v8,(a2)
    mvm.vset v8,zero,t0
    addi a2,a2,GROUP_BYTES
    addi t0,t0,N
    bltu t0,t1,.Lprogram0
    li t0,0
    mv a2,s3
.Lprogram1:
    vle32.v v8,(a2)
    mvm.vset v8,s4,t0
    addi a2,a2,GROUP_BYTES
    addi t0,t0,N
    bltu t0,t1,.Lprogram1
    fence rw,rw

    /* Load source WAR against an ALU overwrite. */
    vle32.v v8,(s0)
    mark 2,1
.balign 64
.global command_p2
command_p2:
    mvm.vl v8,zero,zero
.global target_p2
target_p2:
    vmv.v.i v8,0
    mvm t4,zero,zero
    bnez t4,failed
    mvm.vs v16,zero,zero
    save 16,1
    save 8,2
    mark 2,0

    /* Output reservation RAW: integer ALU adds one to every FP32 bit pattern. */
    job zero,s0
    mark 3,1
.balign 64
.global command_p3
command_p3:
    mvm.vs v8,zero,zero
.global target_p3
target_p3:
    vadd.vi v16,v8,1
    save 16,3
    mark 3,0

    /* Output reservation WAW against an LSQ vector load. */
    job zero,s0
    mark 4,1
.balign 64
.global command_p4
command_p4:
    mvm.vs v8,zero,zero
.global target_p4
target_p4:
    vle32.v v8,(s1)
    save 8,4
    mark 4,0

    /* Output reservation RAW against an LSQ vector store. */
    job zero,s0
    li a3,(OUTPUT+4096*5)
    mark 5,1
.balign 64
.global command_p5
command_p5:
    mvm.vs v8,zero,zero
.global target_p5
target_p5:
    vs8r.v v8,(a3)
    mark 5,0

    /* Load source WAR against an LSQ overwrite, preserving the old input. */
    vle32.v v8,(s0)
    mark 6,1
.balign 64
.global command_p6
command_p6:
    mvm.vl v8,zero,zero
.global target_p6
target_p6:
    vle32.v v8,(s1)
    save 8,6
    mvm t4,zero,zero
    bnez t4,failed
    mvm.vs v16,zero,zero
    save 16,7
    mark 6,0

    /* Short-VL m8 reserves the containing full group, preserving its tails. */
    job zero,s0
    vle32.v v8,(s1)
    cfg m8,1
    mark 7,1
.balign 64
.global command_p7
command_p7:
    mvm.vs v8,zero,zero
    cfg m1,LANES
.global target_p7
target_p7:
    vle32.v v15,(s0)
    save 8,8
    cfg m8,(N-1)
    li t0,1
    mvm.vs v16,zero,t0
    mark 7,0

    /* Fractional group output preserves unused physical-register lanes. */
    job zero,s0
    vle32.v v8,(s1)
    cfg mf2,(LANES/2)
    mark 8,1
.balign 64
.global command_p8
command_p8:
    mvm.vs v8,zero,zero
    cfg m1,LANES
.global target_p8
target_p8:
    save1 8,9
    cfg m8,(N-LANES/2)
    li t0,(LANES/2)
    mvm.vs v16,zero,t0
    mark 8,0

    /* Independent source groups exercise queue and byte credit pressure. */
    cfg m8,N
    vle32.v v0,(s2)
    vle32.v v8,(s3)
    addi a2,s2,GROUP_BYTES
    vle32.v v16,(a2)
    addi a2,s3,GROUP_BYTES
    vle32.v v24,(a2)
    mark 9,1
    li t0,N
.balign 64
.global command_p9
command_p9:
    .rept 3
    mvm.vset v0,zero,zero
    mvm.vset v8,s4,zero
    mvm.vset v16,zero,t0
    mvm.vset v24,s4,t0
    .endr
    mark 9,0

    /* Two distinct matrices and two queued results per array prove routing. */
    mark 10,1
    job zero,s0
    job s4,s1
    job zero,s1
    job s4,s0
    mvm.vs v0,zero,zero
    mvm.vs v8,s4,zero
    mvm.vs v16,zero,zero
    mvm.vs v24,s4,zero
    save 0,10
    save 8,11
    save 16,12
    save 24,13
    mark 10,0

    /* A dynamic illegal array faults while an older program is outstanding. */
    vle32.v v0,(s2)
    vle32.v v8,(s1)
    li a2,1
    slli a2,a2,32
    mark 11,1
.balign 64
.global command_p11
command_p11:
    mvm.vset v0,zero,zero
    .rept 12
    addi t3,t3,1
    .endr
.global target_p11
target_p11:
    mvm.vl v8,a2,zero
    save 8,14
    mark 11,0

    /* Explicit fence retires an outstanding long-latency program. */
    vle32.v v0,(s2)
    mark 12,1
.balign 64
.global command_p12
command_p12:
    mvm.vset v0,zero,zero
.global target_p12
target_p12:
    fence rw,rw
    mark 12,0

    /* VL=0 still acknowledges source capture, but moves no payload. */
    cfg m8,0
    mark 13,1
    mvm.vset v0,zero,zero
    mvm.vl v8,zero,zero
    mvm.vs v16,zero,zero
    mark 13,0

    /* Completion must use saved destination metadata after SEW/LMUL changes. */
    job zero,s0
    mark 14,1
.balign 64
.global command_p14
command_p14:
    mvm.vs v8,zero,zero
    li t2,1
    vsetvli zero,t2,e64,m1,tu,mu
.global target_p14
target_p14:
    save 8,15
    mark 14,0

    /* A representable but nonexistent array reaches the backend Error path. */
    cfg m8,N
    vle32.v v0,(s2)
    vle32.v v8,(s1)
    li a2,2
    mark 15,1
.balign 64
.global command_p15
command_p15:
    mvm.vset v0,zero,zero
    .rept 12
    addi t3,t3,1
    .endr
.global target_p15
target_p15:
    mvm.vl v8,a2,zero
    save 8,16
    mark 15,0

    /* Already captured LSQ stores keep their old bits across an ASQ overwrite. */
    job zero,s0
    vle32.v v8,(s1)
    li a3,(OUTPUT+4096*17)
    mark 16,1
.balign 64
.global issue_p16_store
issue_p16_store:
    vs8r.v v8,(a3)
.global command_p16
command_p16:
    mvm.vs v8,zero,zero
    save 8,18
    mark 16,0

    /* Exit is a final barrier, including a newly submitted program. */
    li t5,0x100000
    li t6,0x5555
.balign 64
.global command_exit
command_exit:
    mvm.vset v0,zero,zero
.global target_exit
target_exit:
    sw t6,0(t5)
    j .
failed:
    li t5,0x100000
    li t6,0x13333
    sw t6,0(t5)
    j .

.balign 64
.global trap_handler
trap_handler:
    addi sp,sp,-32
    sd t0,0(sp)
    sd t1,8(sp)
    sd t2,16(sp)
    li t0,0x901e0000
    lw t1,0(t0)
    addi t1,t1,1
    sw t1,0(t0)
    csrr t2,mcause
    sd t2,8(t0)
    csrr t2,mepc
    sd t2,16(t0)
    addi t2,t2,4
    csrw mepc,t2
    ld t0,0(sp)
    ld t1,8(sp)
    ld t2,16(sp)
    addi sp,sp,32
    mret
