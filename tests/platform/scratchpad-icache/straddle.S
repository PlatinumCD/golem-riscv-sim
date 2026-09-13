.include "common.S"
.option rvc
.globl tile_main
.type tile_main,@function
tile_main:
  .align 6
  c.j straddle_target
  .fill 60, 1, 0
straddle_target:
  /* The aligned 32-bit instruction begins at byte 62 of this line. */
  .2byte 0x0093
  .2byte 0x0000
  PASS
