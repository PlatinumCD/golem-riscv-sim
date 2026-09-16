.include "common.S"
#ifndef REGISTER_ITERATIONS
#define REGISTER_ITERATIONS 100
#endif
.option norvc
.balign 64
.globl tile_main
.type tile_main,@function
tile_main:
  li t0, REGISTER_ITERATIONS
  li t1, 0
.balign 64
1:
  .rept 64
  addi t1, t1, 1
  .endr
  addi t0, t0, -1
  bnez t0, 1b
  li t2, 64 * REGISTER_ITERATIONS
  bne t1, t2, 2f
  PASS
2:
  FAIL
