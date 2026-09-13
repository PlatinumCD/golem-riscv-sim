.include "common.S"
.globl tile_main
.type tile_main,@function
tile_main:
  la t0, input_values
  vsetvli t1, zero, e32, m1, ta, ma
  li t2, 8
  bne t1, t2, rvv_fail
  vle32.v v8, (t0)
  li t3, 7
  vadd.vx v8, v8, t3
  la t0, output_values
  vse32.v v8, (t0)
1:
  lw t2, 0(t0)
  bne t2, t3, rvv_fail
  addi t0, t0, 4
  addi t3, t3, 1
  addi t1, t1, -1
  bnez t1, 1b
  PASS
rvv_fail:
  FAIL

.section .data.input_values,"aw"
.balign 32
input_values:
  .word 0, 1, 2, 3, 4, 5, 6, 7

.section .bss.output_values,"aw",@nobits
.balign 32
output_values:
  .space 32
