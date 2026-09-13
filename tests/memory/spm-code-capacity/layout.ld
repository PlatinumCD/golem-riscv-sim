OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { SPM (rwx) : ORIGIN = 0x90000000, LENGTH = 32K }
SECTIONS {
  .text : ALIGN(64) { KEEP(*(.text.start)) *(.text .text.*) } > SPM
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } > SPM
  .data : ALIGN(16) { __data_start = .; *(.data .data.* .sdata .sdata.*) __data_end = .; } > SPM
  PROVIDE(__global_pointer$ = __data_start + 0x800);
  .bss (NOLOAD) : ALIGN(32) {
    __bss_start = .; *(.bss .bss.* .sbss .sbss.*) *(COMMON)
    . = ALIGN(32); . += 32; __buffer_start = .;
    . += CHUNK_BYTES; __buffer_end = .; . += 32;
    __bss_end = .;
  } > SPM
  __heap_start = .;
  __stack_top = ORIGIN(SPM) + LENGTH(SPM);
  __stack_bottom = __stack_top - 2K;
  __heap_end = __stack_bottom;
  ASSERT(CHUNK_BYTES > 0 && CHUNK_BYTES % 32 == 0, "invalid chunk size")
  ASSERT(__bss_end <= __stack_bottom, "buffer overlaps reserved stack")
  /DISCARD/ : { *(.eh_frame .eh_frame.*) *(.note .note.*) }
}
