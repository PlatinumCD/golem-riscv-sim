.option norvc
.section .text.kernel,"ax"
.balign 64
.global kernel, kernel_end
.type kernel,@function
kernel:
  .rept (CODE_BYTES / 4 - 1)
  addi a0, a0, 1
  .endr
  ret
kernel_end:
.size kernel, .-kernel
