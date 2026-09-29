OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { SPM (rwx) : ORIGIN = 0x90000000, LENGTH = 2M }
SECTIONS
{
    . = ORIGIN(SPM);
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > SPM
    .rodata : { *(.rodata .rodata.*) } > SPM
    .data : { *(.data .data.*) } > SPM
    .sdata : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) } > SPM
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } > SPM
    __stack_top = ORIGIN(SPM) + LENGTH(SPM) - 0x1000;
    ASSERT(. < ORIGIN(SPM) + 0x100000, "guest overlaps shared-bank test data")
}
