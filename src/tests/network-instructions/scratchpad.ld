OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { SPM (rwx) : ORIGIN = 0x90000000, LENGTH = SPM_BYTES }
SECTIONS
{
    . = ORIGIN(SPM);
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > SPM
    .rodata : { *(.rodata .rodata.*) } > SPM
    .data : { *(.data .data.*) } > SPM
    .sdata : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) } > SPM
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } > SPM
    ASSERT(. < 0x90020000, "guest overlaps network source payload")
    .source_a 0x90020000 : { *(.source_a) } > SPM
    .source_b 0x90030000 : { *(.source_b) } > SPM
    .source_c 0x90040000 : { *(.source_c) } > SPM
    ASSERT(LENGTH(SPM) >= 1M, "network fixture requires at least 1 MiB")
    ASSERT(SIZEOF(.source_a) <= 64K && SIZEOF(.source_b) <= 64K && SIZEOF(.source_c) <= 64K, "source buffers overlap")
    __stack_top = ORIGIN(SPM) + LENGTH(SPM) - 0x1000;
}
