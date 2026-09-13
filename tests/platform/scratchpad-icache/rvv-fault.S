.include "common.S"
.globl tile_main
.type tile_main,@function
tile_main:
  la t0, trap_handler
  csrw mtvec, t0
  vsetvli t1, zero, e32, m1, ta, ma
  /* Four elements are in SPM; the fifth faults. Pending element records
   * must drain without merging into the trap handler's instructions. */
  li t0, 0x9003fff0
  vle32.v v8, (t0)
  FAIL

.align 2
trap_handler:
  csrr t0, mcause
  li t1, 5
  bne t0, t1, trap_fail
  csrr t0, mtval
  li t1, 0x90040000
  bne t0, t1, trap_fail
  PASS
trap_fail:
  FAIL
