/* What a saved panic says, read from its text (user/lib/crashinfo.c): the
 * code the kernel's panic screen showed and the panic's own lines.
 *
 * A panic's log is saved by logd as /data/logs/<name>-crash.txt
 * (<crashlog.h>): a few lines saying what it is (the intro, its last line
 * "--------"), then the kernel's log as it was, whose panic part starts at
 * the byte the intro names. The kernel prints these lines in a panic
 * (kernel/debug/panic.c), which is what is read here:
 *     *** JAM OS KERNEL PANIC *** on cpu 1, thread "x" (other CPUs halted: 3)
 *     code JAM-PF-0008: page fault; its digits are the low 16 bits of ...
 *     build Jam OS 0.0.31, git 2079f35
 *     backtrace:
 *       #0  ffffffff80101234  name+0x12
 * The text is untrusted (another kernel wrote it, a disk kept it): every
 * field is cut to its size and made printable, and a code is taken only if
 * it has the code's shape. Pure: works on bytes in memory, for init (the
 * code for its notice), the shell's `crashlog` and utest. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRASH_CODE_MAX 16   /* "JAM-OOM-7F3A" and its NUL */

/* The first panic code ("JAM-<2 or 3 capitals>-<4 hex digits>", after
 * "code ") in text[0..n), into code; false (code "") if there is none (a
 * kernel from before the code). */
bool crash_code(const char *text, size_t n, char code[CRASH_CODE_MAX]);

struct crash_report {
    char     code[CRASH_CODE_MAX];   /* "" if the panic printed none */
    char     what[80];      /* the code line's words: "page fault" */
    char     message[128];  /* the panic's own line (the intro's "The panic: ...") */
    char     where[96];     /* the panic line's "cpu 1, thread "x"" */
    char     boot[96];      /* "boot-0001, which panicked after 1.477 s (panic 1 in a row)" */
    char     build[64];     /* "Jam OS 0.0.31, git 2079f35", "" if not printed */
    uint64_t panic_at;      /* the panic's first byte, from the text's start */
    uint64_t text_at;       /* the text's first byte in the file */
};

/* The intro (the file's first n bytes; give it at least to the
 * "--------" line, CRASH_INTRO_MAX is enough): the message, the boot and
 * where the text and the panic in it start. false if it isn't a crash
 * log. *r is zeroed first. */
#define CRASH_INTRO_MAX 2048
bool crash_read_intro(const char *s, size_t n, struct crash_report *r);
/* The panic's lines (n bytes of the text from panic_at): the code, its
 * words, where and the build. */
void crash_read_panic(const char *s, size_t n, struct crash_report *r);
/* The backtrace's frames in the panic's lines, one at a time: from *at,
 * the next "#N ..." line after "backtrace:" (cut and made printable into
 * line, size bytes); false when there are no more. *at starts at 0. */
bool crash_next_frame(const char *s, size_t n, size_t *at, char *line, size_t size);
