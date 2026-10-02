.text
.balign 64
.globl main
main:
    mv s11, ra
    la t0, trap_handler
    csrw mtvec, t0
    la a0, input
    li a1, 0x90110000
    /* Independent destinations, widths and signed/unsigned extension. */
    lb t0, 0(a0)
    lbu t1, 0(a0)
    lh t2, 0(a0)
    lhu t3, 0(a0)
    lw t4, 0(a0)
    lwu t5, 0(a0)
    ld t6, 0(a0)
    ld a2, 8(a0)
    ld zero, 0(a0)
    sd t0, 0(a1)
    sd t1, 8(a1)
    sd t2, 16(a1)
    sd t3, 24(a1)
    sd t4, 32(a1)
    sd t5, 40(a1)
    sd t6, 48(a1)
    sd a2, 56(a1)
    /* Register WAW and load-to-branch RAW. */
    ld t0, 0(a0)
    li t0, 23
    sd t0, 64(a1)
    ld t1, 0(a0)
    ld t1, 8(a0)
    sd t1, 72(a1)
    lbu t2, 0(a0)
    li t3, 0x98
    bne t2, t3, bad
    /* Captured stores, same-address RAW and partial overlap. */
    li t0, 41
    sd t0, 80(a1)
    li t0, 99
    sd t0, 88(a1)
    ld t1, 80(a1)
    sd t1, 96(a1)
    li a3, 0x90120000
    ld t0, 8(a0)
    sd t0, 0(a3)
    li t1, 0xaabb
    sh t1, 2(a3)
    ld t2, 0(a3)
    sd t2, 104(a1)
    /* WAR: capture the older load before a younger store changes memory. */
    ld t2, 0(a3)
    li t3, 0x1234
    sd t3, 0(a3)
    sd t2, 112(a1)
    /* Vector/scalar ordered visibility in both directions. */
    vsetivli zero, 4, e32, m1, tu, mu
    vmv.v.i v8, 7
    vse32.v v8, (a3)
    ld t0, 0(a3)
    sd t0, 120(a1)
    li t1, 0x0000000900000008
    sd t1, 0(a3)
    vle32.v v9, (a3)
    addi t2, a1, 128
    vse32.v v9, (t2)
    ld t0, 0(a3)
    vmv.v.i v10, 2
    vse32.v v10, (a3)
    sd t0, 144(a1)
    vle32.v v11, (a3)
    li t1, -1
    sd t1, 0(a3)
    addi t2, a1, 152
    vse32.v v11, (t2)
    /* FP destination hazards and FP32 NaN boxing. */
    flw ft0, 16(a0)
    fmv.x.d t0, ft0
    sd t0, 168(a1)
    fld ft1, 24(a0)
    fadd.d ft2, ft1, ft1
    fsd ft2, 176(a1)
    flw ft3, 16(a0)
    fadd.s ft4, ft3, ft3
    fsw ft4, 184(a1)
    li s0, 0x90120000
    fld fs0, 24(a0)
    c.fsd fs0, 8(s0)
    c.fld fs1, 8(s0)
    fsd fs1, 192(a1)
    li s1, 81
    c.sd s1, 0(s0)
    c.ld s1, 0(s0)
    sd s1, 200(a1)
    c.sdsp s1, 0(sp)
    c.ldsp s1, 0(sp)
    sd s1, 208(a1)
    c.fsdsp fs1, 8(sp)
    c.fldsp fs1, 8(sp)
    fsd fs1, 216(a1)
    /* RVV scalar operands/results share the GPR/FPR hazard scoreboard. */
    vmset.m v0
    ld t0, 8(a0)
    vcpop.m t0, v0
    sd t0, 272(a1)
    ld t1, 8(a0)
    vfirst.m t1, v0
    sd t1, 280(a1)
    lbu t0, 0(a0)
    vmv.v.x v12, t0
    ld t1, 8(a0)
    vmv.x.s t1, v12
    sd t1, 288(a1)
    flw ft0, 16(a0)
    vfmv.v.f v12, ft0
    fld ft1, 24(a0)
    vfmv.f.s ft1, v12
    fmv.x.d t0, ft1
    sd t0, 296(a1)
    /* Misaligned and page-crossing accesses use the synchronous path. */
    lw t0, 1(a0)
    sd t0, 224(a1)
    li a4, 0x90120ff8
    ld t0, 0(a0)
    ld t1, 8(a0)
    sd t0, 0(a4)
    sd t1, 8(a4)
    ld t2, 6(a4)
    sd t2, 232(a1)
    /* Pending stores/loads must retire before a precise fault handler. */
    li t0, 111
    sd t0, 240(a1)
    ld s2, 8(a0)
    li t6, 0x90200000
    .option push
    .option norvc
.globl fault_point
fault_point:
    ld t5, 0(t6)
    .option pop
    sd s2, 248(a1)
    fence rw, rw
    /* Warm and then time unchanged scalar instruction streams. */
    la a0, input
    li a1, 0x90130000
    li s0, 2
    call load_stream
    li s0, 2
    call store_stream
    fence rw, rw
    li a0, 100
    li a1, 1
    call benchmark_marker
    la a0, input
    li s0, 64
    call load_stream
    li a0, 100
    li a1, 0
    call benchmark_marker
    li a0, 101
    li a1, 1
    call benchmark_marker
    li a1, 0x90130000
    li s0, 64
    call store_stream
    li a0, 101
    li a1, 0
    call benchmark_marker
    /* Serialized pointer chasing remains a dependency-limited case. */
    li a0, 102
    li a1, 1
    call benchmark_marker
    li t0, 0x90130000
    sd t0, 0(t0)
    li s0, 64
1:  ld t0, 0(t0)
    addi s0, s0, -1
    bnez s0, 1b
    li a0, 102
    li a1, 0
    call benchmark_marker
    li a0, 0
    mv ra, s11
    ret
bad:
    li a0, 10
    mv ra, s11
    ret
.balign 64
load_stream:
    .set off, 0
    .irp reg,t0,t1,t2,t3,t4,t5,t6,a2,a3,a4,a5,a6,a7,s2,s3,s4
    ld \reg, off(a0)
    .set off, off+8
    .endr
    addi s0, s0, -1
    bnez s0, load_stream
    ret
.balign 64
store_stream:
    .set off, 0
    .irp reg,t0,t1,t2,t3,t4,t5,t6,a2,a3,a4,a5,a6,a7,s2,s3,s4
    sd \reg, off(a1)
    .set off, off+8
    .endr
    addi s0, s0, -1
    bnez s0, store_stream
    ret
.balign 64
.globl trap_handler
trap_handler:
    li t3, 0x90110000
    csrr t4, mcause
    sd t4, 256(t3)
    csrr t4, mtval
    sd t4, 264(t3)
    csrr t4, mepc
    addi t4, t4, 4
    csrw mepc, t4
    mret
.data
.balign 4096
input:
    .quad 0x88776655fedcba98
    .quad 0x1122334455667788
    .quad 0x000000003fc00000
    .quad 0x4004000000000000
    .rept 12
    .quad 0xabcdef0123456789
    .endr
