.include "common.S"
.option norvc
.globl tile_main
.type tile_main,@function
tile_main:
  la t0, trigger_handler
  csrw mtvec, t0
  csrw tselect, zero
  li t1, 2
  li t2, 0
  la t3, warmed
  jalr ra, t3
  /* Arm a type-3 timer well beyond this sequence. Merely configuring this
   * trigger must veto grouping, even when QEMU's itrigger_enabled is false. */
  li t0, 0x3000000000fffe00
  csrw tdata1, t0
  csrr t0, tdata1
  srli t0, t0, 60
  li t1, 3
  bne t0, t1, trigger_fail
  jalr ra, t3
  li t0, 0x2000000000000000
  csrw tdata1, t0
  PASS
.balign 64
warmed:
  .rept 64
  addi t2, t2, 1
  .endr
  ret
.balign 4
trigger_handler:
  csrr t0, mcause
  li t1, 3
  bne t0, t1, trigger_fail
  li t0, 0x2000000000000000
  csrw tdata1, t0
  PASS
trigger_fail:
  FAIL
