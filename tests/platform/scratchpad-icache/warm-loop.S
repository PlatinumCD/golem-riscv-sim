.include "common.S"
.globl tile_main
.type tile_main,@function
tile_main:
  li t0, 256
1: addi t0, t0, -1
  bnez t0, 1b
  PASS
