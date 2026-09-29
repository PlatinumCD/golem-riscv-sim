/* Measurement plumbing. a0=phase, a1=count, a2=x, a3=y, a4=out.
 * Caller-preserved state is saved/restored outside both marker boundaries.
 */
.text
.balign 4
.globl run_phase
.type run_phase, @function
run_phase:
    addi sp, sp, -64
    sd ra, 56(sp)
    sd s0, 48(sp)
    sd s1, 40(sp)
    sd s2, 32(sp)
    sd s3, 24(sp)
    sd s4, 16(sp)
    sd s5, 8(sp)
    mv s0, a0
    mv s1, a1
    mv s2, a2
    mv s3, a3
    mv s4, a4
    li s5, 0
    li a1, 1
    call benchmark_marker
    beqz s1, 1f
    mv a0, s2
    mv a1, s3
    mv a2, s4
    mv a3, s1
    call transfer_kernel
    mv s5, a0
1:
    mv a0, s0
    li a1, 0
    call benchmark_marker
    mv a0, s5
    ld s5, 8(sp)
    ld s4, 16(sp)
    ld s3, 24(sp)
    ld s2, 32(sp)
    ld s1, 40(sp)
    ld s0, 48(sp)
    ld ra, 56(sp)
    addi sp, sp, 64
    ret
.size run_phase, .-run_phase
