.include "common.S"
.option norvc
.globl tile_main
.type tile_main,@function
tile_main:
  call kernel
  li t1, 1
  bne a0, t1, reload_fail
  la t0, kernel
  li t1, 0x00200513       /* addi a0, zero, 2 */
  sw t1, 0(t0)
  fence.i
  call kernel
  li t1, 2
  bne a0, t1, reload_fail
  PASS
reload_fail:
  FAIL

.balign 64
kernel:
  addi a0, zero, 1
  ret
