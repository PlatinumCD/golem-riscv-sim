OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Code, data and stack share SPM. Override these with --defsym before -T. */
__spm_bytes = DEFINED(SPM_BYTES) ? SPM_BYTES : 256K;
__code_offset = DEFINED(SPM_CODE_OFFSET) ? SPM_CODE_OFFSET : 0;
__stack_size = DEFINED(STACK_BYTES) ? STACK_BYTES : 16K;
MEMORY
{
    TILE_SCRATCHPAD (rwx) : ORIGIN = 0x90000000 + __code_offset,
                          LENGTH = __spm_bytes - __code_offset
}

SECTIONS
{
    . = ORIGIN(TILE_SCRATCHPAD);

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > TILE_SCRATCHPAD

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } > TILE_SCRATCHPAD

    .data : ALIGN(16)
    {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        __data_end = .;
    } > TILE_SCRATCHPAD

    PROVIDE(__global_pointer$ = __data_start + 0x800);

    .bss (NOLOAD) : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > TILE_SCRATCHPAD

    . = ALIGN(16);
    __heap_start = .;
    __stack_top = ORIGIN(TILE_SCRATCHPAD) + LENGTH(TILE_SCRATCHPAD);
    __stack_bottom = __stack_top - __stack_size;
    __heap_end = __stack_bottom;

    ASSERT(__heap_start <= __heap_end, "scratchpad image overlaps its stack")
    ASSERT(__code_offset < __spm_bytes, "code offset is outside scratchpad")

    /DISCARD/ :
    {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
