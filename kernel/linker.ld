/* Jam OS kernel: higher-half at the top 2 GiB (-mcmodel=kernel).
 *
 * .ksyms (the symbol table for backtraces) is deliberately LAST and in its
 * own segment: the build links twice, and filling in .ksyms on the second
 * link must not move any other symbol. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
    ksyms           PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : { *(.text .text.*) } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
        /* User-copy fixups: (faulting instruction, resume address) pairs. */
        . = ALIGN(8);
        __ex_table_start = .;
        KEEP(*(__ex_table))
        __ex_table_end = .;
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : { *(.data .data.*) } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    __data_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __ksyms_start = .;
    .ksyms : { KEEP(*(.ksyms .ksyms.*)) } :ksyms
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
