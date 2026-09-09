OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    TILE_RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16M
}
__stack_size = 64K;

SECTIONS
{
    . = ORIGIN(TILE_RAM);

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > TILE_RAM

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } > TILE_RAM

    .data : ALIGN(16)
    {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        __data_end = .;
    } > TILE_RAM

    PROVIDE(__global_pointer$ = __data_start + 0x800);

    .bss (NOLOAD) : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > TILE_RAM

    . = ALIGN(16);
    __heap_start = .;
    __stack_top = ORIGIN(TILE_RAM) + LENGTH(TILE_RAM);
    __stack_bottom = __stack_top - __stack_size;
    __heap_end = __stack_bottom;

    ASSERT(__heap_start <= __heap_end, "tile image overlaps its stack")

    /DISCARD/ :
    {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
