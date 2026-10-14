/* bigprog's table (main.c): BIG_PAGES pages of constant data, page i
 * holding the 64-bit word i in each of its 512 places. The boot image's
 * bin/bigprog is built with 16; the Makefile's $(BUILD)/bigprog/bigprog
 * with BIGPROG_PAGES (300 MiB), assembled here when a test asks for it:
 * the big program is generated, never kept in the tree. In .rodata, so it
 * is in the program's read-only segment, mapped from its VMO. */
#ifndef BIG_PAGES
#define BIG_PAGES 16
#endif

    .section .rodata.bigprog, "a"
    .balign 4096
    .globl bigprog_table
bigprog_table:
    .set page, 0
    .rept BIG_PAGES
    .fill 512, 8, page
    .set page, page + 1
    .endr
    .globl bigprog_table_end
bigprog_table_end:

    .section .note.GNU-stack, "", @progbits
