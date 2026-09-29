.section .text
.option push
.option norvc
.balign 64
.global hot_loop
.type hot_loop, @function
hot_loop:
    mv t0, a0
    li a0, 0
1:  addi a0, a0, 3
    addi t0, t0, -1
    bnez t0, 1b
    ret
.size hot_loop, .-hot_loop

.balign 64
.global conflict_loop
.type conflict_loop, @function
conflict_loop:
    mv t0, a0
    li a0, 0
    j conflict_a
.balign 64
.global conflict_a
conflict_a:
    addi a0, a0, 1
    j conflict_b
.balign 64
.global conflict_b
conflict_b:
    addi a0, a0, 2
    j conflict_c
.balign 64
.global conflict_c
conflict_c:
    addi a0, a0, 3
    addi t0, t0, -1
    bnez t0, conflict_a
    ret
.size conflict_loop, .-conflict_loop

/* The call lands directly on a 32-bit instruction spanning two 64-byte
   cache lines. Padding is data and is never executed. */
.balign 64
.zero 62
.global boundary_instruction
.type boundary_instruction, @function
boundary_instruction:
    addi a0, zero, 37
    ret
.size boundary_instruction, .-boundary_instruction

/* With four-byte cache lines this crossing stays inside a single 32-byte
   SPM ordering line, exercising sequential refill release of that line. */
.balign 64
.zero 2
.global small_boundary_instruction
.type small_boundary_instruction, @function
small_boundary_instruction:
    addi a0, zero, 41
    ret
.size small_boundary_instruction, .-small_boundary_instruction

.balign 64
.global modified_code
.type modified_code, @function
modified_code:
    addi a0, zero, 17
    ret
.size modified_code, .-modified_code
.option pop
