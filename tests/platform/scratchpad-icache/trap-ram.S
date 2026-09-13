.include "common.S"
.globl tile_main
.type tile_main,@function
tile_main:
  la t0, trap_handler
  csrw mtvec, t0
  li t0, 0x80000000
  lw t1, 0(t0)
  /* Returning means the forbidden address was incorrectly mapped. */
  FAIL

.align 2
trap_handler:
  csrr t0, mcause
  li t1, 5                    /* load access fault */
  bne t0, t1, trap_fail
  csrr t0, mtval
  li t1, 0x80000000
  bne t0, t1, trap_fail
  la a0, pass_marker
  call uart_puts
  li a0, 0
  tail platform_exit
trap_fail:
  li a0, 2
  tail platform_exit
