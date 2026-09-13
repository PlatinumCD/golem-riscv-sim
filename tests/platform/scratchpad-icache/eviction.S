.include "common.S"
.option norvc
.globl tile_main
.type tile_main,@function
tile_main:
  la t0, eviction_body
  jalr ra, t0
  jalr ra, t0
  PASS
.section .text.eviction_body,"ax"
.align 6
eviction_body:
  /* 20 KiB of code, traversed twice, exceeds the 8 KiB cache twice. */
  .rept 5120
  addi t1, t1, 1
  .endr
  ret
