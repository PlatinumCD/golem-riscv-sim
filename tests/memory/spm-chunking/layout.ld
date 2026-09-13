OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { SPM (rwx) : ORIGIN = 0x90000000, LENGTH = 16K }
SECTIONS {
  .text : ALIGN(16) { KEEP(*(.text.start)) *(.text .text.*) } > SPM
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } > SPM
  .data : ALIGN(16) {
    __data_start = .; *(.data .data.* .sdata .sdata.*) __data_end = .;
  } > SPM
  PROVIDE(__global_pointer$ = __data_start + 0x800);
  .bss (NOLOAD) : ALIGN(32) {
    __bss_start = .; *(.bss .bss.* .sbss .sbss.*) *(COMMON)
    . = ALIGN(8); __bss_end = .;
  } > SPM
  __heap_start = ALIGN(16);
  __stack_top = ORIGIN(SPM) + LENGTH(SPM);
  __stack_bottom = __stack_top - 2K;
  __heap_end = __stack_bottom;
  ASSERT(__heap_start <= __stack_bottom, "chunking fixture exceeds SPM capacity")
  /DISCARD/ : { *(.eh_frame .eh_frame.*) *(.note .note.*) }
}
