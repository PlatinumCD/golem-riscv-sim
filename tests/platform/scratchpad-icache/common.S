.section .text
.align 2
.macro PASS
  la a0, pass_marker
  call uart_puts
  li a0, 0
  tail platform_exit
.endm
.macro FAIL
  li a0, 1
  tail platform_exit
.endm
.section .rodata.scratchpad_icache,"a"
pass_marker:
  .asciz "SCRATCHPAD_ICACHE_PASS\n"
.section .text
