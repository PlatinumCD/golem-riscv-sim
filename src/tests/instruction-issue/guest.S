.text
.option norvc
.macro measure name, id
    call \name
    li a0, \id
    li a1, 1
    call benchmark_marker
    call \name
    li a0, \id
    li a1, 0
    call benchmark_marker
.endm
.globl main
main:
    mv s11, ra
    measure independent, 1
    measure dependent, 2
    measure mixed_rvv, 3
    measure pointer_chain, 4
    measure mixed_memory, 5
    measure multiply_chain, 6
    measure vector_chain, 7
    li a0, 0
    jr s11

.balign 64
independent:
    li a2, 1
    li a3, 2
    li a4, 3
    li a5, 4
.globl independent_begin, independent_end
independent_begin:
    .rept 64
    add t2, a2, a3
    add t3, a4, a5
    .endr
independent_end:
    li t0, 0x90100000
    sd t2, 0(t0)
    sd t3, 8(t0)
    ret

.balign 64
dependent:
    li t2, 0
.globl dependent_begin, dependent_end
dependent_begin:
    .rept 128
    addi t2, t2, 1
    .endr
dependent_end:
    li t0, 0x90100000
    sd t2, 16(t0)
    ret

.balign 64
mixed_rvv:
    vsetivli zero, 4, e32, m1, ta, ma
    vmv.v.i v1, 1
    vmv.v.i v2, 2
    li t2, 0
.globl mixed_rvv_begin, mixed_rvv_end
mixed_rvv_begin:
    .rept 64
    vadd.vv v8, v1, v2
    addi t2, t2, 1
    .endr
mixed_rvv_end:
    li t0, 0x90100000
    sd t2, 24(t0)
    addi t0, t0, 128
    vse32.v v8, (t0)
    ret

.balign 64
pointer_chain:
    la t2, node
.globl pointer_chain_begin, pointer_chain_end
pointer_chain_begin:
    .rept 32
    ld t2, 0(t2)
    .endr
pointer_chain_end:
    la t0, node
    sub t2, t2, t0
    li t0, 0x90100000
    sd t2, 32(t0)
    ret

.balign 64
mixed_memory:
    la a2, values
    li a3, 0
.globl mixed_memory_begin, mixed_memory_end
mixed_memory_begin:
    .rept 8
    ld t2, 0(a2)
    addi a3, a3, 1
    ld t3, 8(a2)
    addi a3, a3, 1
    ld t4, 16(a2)
    addi a3, a3, 1
    ld t5, 24(a2)
    addi a3, a3, 1
    .endr
mixed_memory_end:
    add t2, t2, t3
    add t2, t2, t4
    add t2, t2, t5
    li t0, 0x90100000
    sd t2, 40(t0)
    sd a3, 48(t0)
    ret

.balign 64
multiply_chain:
    li t2, 1
    li t3, 1
.globl multiply_chain_begin, multiply_chain_end
multiply_chain_begin:
    .rept 32
    mul t2, t2, t3
    .endr
multiply_chain_end:
    li t2, 17                  # WAW after the last multiply must wait too.
    li t0, 0x90100000
    sd t2, 56(t0)
    ret

.balign 64
vector_chain:
    vsetivli zero, 4, e32, m1, ta, ma
    vmv.v.i v1, 1
    vmv.v.i v8, 0
.globl vector_chain_begin, vector_chain_end
vector_chain_begin:
    .rept 32
    vadd.vv v8, v8, v1
    .endr
vector_chain_end:
    li t0, 0x90100090
    vse32.v v8, (t0)
    ret

.data
.balign 64
node: .dword node
values: .dword 1,2,3,4
