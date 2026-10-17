/* Jam OS POSIX programs (musl, user/posix/): static ELF64 at 0x400000, or
 * at __jamos_image_base when the link defines it (the drivers'
 * --jamos-image-base, a port's `image-base`: Chromium's programs sit above
 * 4 GiB; libc.a's code is position-independent, so it links anywhere),
 * user/linker.ld's three segments with what musl needs on top:
 *
 *   - the ELF and program headers at the start of the first segment, so
 *     they are mapped and the start (user/posix/crt/start.c) can point
 *     AT_PHDR at them (__ehdr_start), where musl's __init_tls finds
 *     PT_TLS;
 *   - PT_TLS over .tdata and .tbss: thread-local storage (errno, ...),
 *     whose image sits in the writable segment;
 *   - .init and .fini (crti.o, crtn.o) and the init and fini arrays
 *     musl's start and exit walk (__init_array_start, ...), with the
 *     .ctors and .dtors sections in them: the cross compiler (x86_64-elf
 *     GCC) puts constructors there, not in .init_array, and without these
 *     lines they were dropped and never run. ld sorts both kinds by their
 *     priority (a .ctors.N's priority is 65535 - N), as its own default
 *     script does.
 *
 *   - the unwind tables, .eh_frame and its index .eh_frame_hdr, in the
 *     read-only segment, the index under a PT_GNU_EH_FRAME: C++
 *     exceptions (tools/jamos-clang++) unwind through them, libunwind
 *     finding the index by dl_iterate_phdr; .gcc_except_table (the
 *     catch sites) beside them. A C program built without unwind tables
 *     (the tree's own) has none to keep; jamos-cc's ports have theirs
 *     (about 2% of posixtest).
 *
 * Each segment starts on its own page, so every page is read+execute,
 * read-only or read+write, never writable and executable (W^X). The
 * program's list (<wants.h>, .jamos.wants) sits in the read-only segment
 * under a PT_NOTE, as in user/linker.ld, and the GNU build-id note the
 * link drivers ask for (--build-id: a hash of the program's contents, so
 * the same input links to the same bytes; crash reports and Chromium's
 * ElfReader key a program by it) after it under one of its own, which a
 * reader of the list skips (another name). Sections are kept or dropped by
 * --gc-sections from _start; the ones nothing refers to that must stay
 * are KEEP. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R X, with the headers */
    rodata PT_LOAD FLAGS(4);                 /* R */
    data   PT_LOAD FLAGS(6);                 /* R W */
    tls    PT_TLS FLAGS(4);                  /* the TLS image, inside data */
    wants  PT_NOTE FLAGS(4);                 /* the program's list */
    buildid PT_NOTE FLAGS(4);                /* the GNU build-id note */
    eh     PT_GNU_EH_FRAME FLAGS(4);         /* .eh_frame_hdr, inside rodata */
}

SECTIONS
{
    . = (DEFINED(__jamos_image_base) ? __jamos_image_base : 0x400000) + SIZEOF_HEADERS;
    .init : { KEEP(*(.init)) } :text
    .text : {
        *(.text.start)
        *(.text .text.*)
    } :text
    .fini : { KEEP(*(.fini)) } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .jamos.wants : { KEEP(*(.jamos.wants)) } :rodata :wants
    .note.gnu.build-id : { KEEP(*(.note.gnu.build-id)) } :rodata :buildid
    .eh_frame_hdr : { *(.eh_frame_hdr) } :rodata :eh
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata

    . = ALIGN(4096);
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss : { *(.tbss .tbss.*) } :data :tls
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .data : { *(.data .data.*) } :data
    .got : { *(.got) *(.got.plt) } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : { *(.note .note.*) *(.comment) *(.interp) *(.dynamic) }
}
