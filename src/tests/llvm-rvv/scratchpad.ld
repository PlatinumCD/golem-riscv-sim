OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { SPM (rwx) : ORIGIN = 0x90000000, LENGTH = 32M }
SECTIONS
{
    .input_x 0x900fffc0 : { KEEP(*(.rodata.input_x)) } > SPM
    .input_y 0x901fffc0 : { KEEP(*(.rodata.input_y)) } > SPM
    .output 0x902fffc0 : { KEEP(*(.data.output)) } > SPM
    .warm_output 0x903fffc0 : { KEEP(*(.data.warm_output)) } > SPM
    ASSERT(SIZEOF(.input_x) <= 0x100000, "input x overlaps input y")
    ASSERT(SIZEOF(.input_y) <= 0x100000, "input y overlaps output")
    ASSERT(SIZEOF(.output) <= 0x100000, "output overlaps warm output")
    ASSERT(SIZEOF(.warm_output) <= 0x100000, "warm output overlaps mailbox")

    .text ORIGIN(SPM) : { KEEP(*(.text.start)) *(.text .text.*) } > SPM
    .rodata ALIGN(ADDR(.text) + SIZEOF(.text), 16) : { *(.rodata .rodata.* .srodata .srodata.*) } > SPM
    .data ALIGN(ADDR(.rodata) + SIZEOF(.rodata), 16) : { *(.data .data.*) } > SPM
    .sdata ALIGN(ADDR(.data) + SIZEOF(.data), 16) : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) } > SPM
    .bss ALIGN(ADDR(.sdata) + SIZEOF(.sdata), 16) (NOLOAD) : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } > SPM
    ASSERT(. <= 0x900fffc0, "code/data overlap input guards")
    __stack_top = 0x91fff000;
}
