/* Jam OS user programs: static ELF64 at 0x400000 (no ASLR yet).
 *
 * Three segments, each starting on its own page, so every page is either
 * read+execute, read-only or read+write, never writable and executable
 * (W^X; the kernel's ELF parser refuses a W+X segment). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R */
    data   PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
    . = 0x400000;
    .text : {
        *(.text.start)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) *(.interp) *(.dynamic) }
}
