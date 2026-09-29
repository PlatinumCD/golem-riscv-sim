#ifndef VLEN_BITS
#define VLEN_BITS 256
#endif
#define VECTOR_BYTES (VLEN_BITS / 8)
.text
.balign 4
.macro LOAD24
.irp n,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    vle32.v v\n, (a0)
    addi a0, a0, VECTOR_BYTES
.endr
.endm
.macro STORE24
.irp n,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    vse32.v v\n, (a0)
    addi a0, a0, VECTOR_BYTES
.endr
.endm
.globl loads24
loads24:
    LOAD24
    ret
.globl stores24
stores24:
    STORE24
    ret
.globl load_pair
load_pair:
    vle32.v v8, (a0)
    addi a0, a0, VECTOR_BYTES
    vle32.v v9, (a0)
    fence rw, rw
    ret
.globl raw_kernel
raw_kernel:
    vle32.v v8, (a0)
    vse32.v v8, (a1)
    vadd.vi v9, v8, 1
    addi a1, a1, VECTOR_BYTES
    vse32.v v9, (a1)
    ret
.globl waw_kernel
waw_kernel:
    vle32.v v8, (a0)
    addi a0, a0, VECTOR_BYTES
    vle32.v v8, (a0)
    vse32.v v8, (a1)
    ret
.globl snapshot_kernel
snapshot_kernel:
    vse32.v v8, (a1)
    addi a0, a0, VECTOR_BYTES
    vle32.v v8, (a0)
    addi a1, a1, VECTOR_BYTES
    vse32.v v8, (a1)
    ret
.globl memory_raw_kernel
memory_raw_kernel:
    vse32.v v8, (a0)
    vle32.v v10, (a0)
    vse32.v v10, (a1)
    ret
.globl partial_raw_kernel
partial_raw_kernel:
    vse32.v v8, (a0)
    addi a0, a0, 4
    vle32.v v10, (a0)
    vse32.v v10, (a1)
    ret
.globl memory_waw_kernel
memory_waw_kernel:
    vse32.v v8, (a0)
    addi a0, a0, 4
    vse32.v v9, (a0)
    ret
.globl memory_war_kernel
memory_war_kernel:
    vle32.v v8, (a0)
    vse32.v v9, (a0)
    vse32.v v8, (a1)
    ret
.globl alu_snapshot_kernel
alu_snapshot_kernel:
    vse32.v v8, (a0)
    vmv.v.i v8, 7
    addi a0, a0, VECTOR_BYTES
    vse32.v v8, (a0)
    ret
.globl drain_kernel
/* MODE: 1=fence, 2=fence.i, 3=exit, 4=precise fault. */
drain_kernel:
    STORE24
.globl drain_point
drain_point:
#if MODE == 1
    fence rw, rw
#elif MODE == 2
    fence.i
#elif MODE == 3
    li t0, 0x100000
    li t1, 0x5555
    sw t1, 0(t0)
1:  j 1b
#elif MODE == 4
    li a0, 0x90200000
    vle32.v v8, (a0)
#endif
    ret

.balign 64
.globl fetch_fault_kernel
fetch_fault_kernel:
    /* The virt PCIe MMIO aperture is executable through QEMU's MMIO fetch
     * path; use the genuinely unmapped end of the scratchpad instead. */
    li t0, 0x90200000
    vle32.v v8, (a0)
    vse32.v v9, (a1)
.globl fetch_fault_jump
fetch_fault_jump:
    jalr zero, t0, 0
.globl fetch_fault_resume
fetch_fault_resume:
    ret
