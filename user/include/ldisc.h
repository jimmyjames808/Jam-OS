/* A terminal's line discipline (libos: user/lib/ldisc.c; docs/M13-PLAN.md,
 * "The terminal layer"): POSIX's termios between the keys a terminal types
 * and the programs that read it. The console keeps one per terminal (so
 * `stty` in one program holds for the next, and what was typed ahead stays
 * with the terminal); libc's terminal descriptor uses its read rules
 * (ld_read_deadline). Pure: bytes in, bytes out, no system calls, so the
 * tests are utests.
 *
 * Input: each byte typed goes through ld_input, which edits the line
 * (ICANON: erase, word erase, kill, reprint, end of file, the literal next
 * of IEXTEN), maps CR and NL (ICRNL, INLCR, IGNCR, ISTRIP), echoes (ECHO,
 * ECHOE, ECHOK, ECHOKE, ECHONL, ECHOCTL) through the echo callback, and
 * says when ISIG's keys were typed (the caller raises the signal; the
 * input is flushed unless NOFLSH). What a reader may take waits in the
 * ready queue as records: in canonical mode one line per record (an empty
 * record is end of file: a VEOF typed at the start of a line), in raw mode
 * the bytes as they came, joined into the last record until it is taken.
 * A canonical read never returns more than one record; the console sends
 * each record as one message on the reader's input pipe.
 *
 * Output: ld_output applies OPOST (ONLCR, OCRNL) to what a program writes
 * and keeps the column the terminal's cursor is at, which erasing a tab
 * needs (the echo goes through it too).
 *
 * The termios fields are Linux's, with Linux's bit values and c_cc
 * indices, so libc passes musl's struct termios through unchanged. Bits
 * this module has no meaning for (the baud rate and character size in
 * c_cflag, IXON, parity, ...) are kept as they were set and change
 * nothing, as on a Linux pseudo-terminal. A c_cc entry of 0 is disabled
 * (_POSIX_VDISABLE). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LD_NCCS 32   /* c_cc's entries (musl's NCCS) */

/* c_cc indices. */
enum {
    LD_VINTR = 0, LD_VQUIT = 1, LD_VERASE = 2, LD_VKILL = 3, LD_VEOF = 4, LD_VTIME = 5,
    LD_VMIN = 6, LD_VSWTC = 7, LD_VSTART = 8, LD_VSTOP = 9, LD_VSUSP = 10, LD_VEOL = 11,
    LD_VREPRINT = 12, LD_VDISCARD = 13, LD_VWERASE = 14, LD_VLNEXT = 15, LD_VEOL2 = 16,
};

/* c_iflag. */
#define LD_ISTRIP 0000040u
#define LD_INLCR  0000100u
#define LD_IGNCR  0000200u
#define LD_ICRNL  0000400u
#define LD_IUTF8  0040000u   /* erase takes a whole UTF-8 character */
/* c_oflag. */
#define LD_OPOST  0000001u
#define LD_ONLCR  0000004u
#define LD_OCRNL  0000010u
/* c_cflag: kept, never used (B38400 | CS8 | CREAD by default). */
#define LD_CFLAG_DEFAULT 0000277u
/* c_lflag. */
#define LD_ISIG    0000001u
#define LD_ICANON  0000002u
#define LD_ECHO    0000010u
#define LD_ECHOE   0000020u
#define LD_ECHOK   0000040u
#define LD_ECHONL  0000100u
#define LD_NOFLSH  0000200u
#define LD_ECHOCTL 0001000u
#define LD_ECHOKE  0004000u
#define LD_IEXTEN  0100000u

/* The longest line canonical mode edits (POSIX's MAX_CANON), its
 * delimiter included: characters past it are dropped. */
#define LD_MAX_CANON 4096u
/* Typed input waiting for a reader: bytes, and records. Past either,
 * what is typed is dropped (as Linux's full queue drops it). */
#define LD_READY_MAX (16u << 10)
#define LD_RECS_MAX  256u
/* A raw record grows to at most this many bytes (one message). */
#define LD_REC_MAX   4096u
/* The deadline that never comes (DEADLINE_NEVER). */
#define LD_NEVER     UINT64_MAX

/* A terminal's settings: Linux's struct termios, field for field (but
 * c_line, which is always 0). */
struct ld_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  cc[LD_NCCS];
    uint32_t ispeed, ospeed;   /* kept for cfgetispeed's sake; never used */
};

/* What ISIG's keys asked for (ld_input's result). */
enum ld_sig { LD_SIG_NONE, LD_SIG_INT, LD_SIG_QUIT, LD_SIG_SUSP };

/* The echo: bytes to show on the terminal, output processing applied. */
typedef void ld_echo_fn(void *ctx, const uint8_t *p, size_t n);

/* One terminal's line discipline. Set it up with ld_init; the fields are
 * its own. */
struct ldisc {
    struct ld_termios t;          /* the settings */
    ld_echo_fn *echo;             /* where the echo goes */
    void       *echo_ctx;
    uint8_t  line[LD_MAX_CANON];  /* the line being edited (ICANON) */
    uint32_t nline;
    uint32_t col;                 /* the terminal's column, as output moved it */
    uint32_t canon_col;           /* the column the line being edited started at */
    bool     lnext;               /* the last byte was VLNEXT: the next is literal */
    bool     open;                /* the last record is raw and may still grow */
    uint8_t  ready[LD_READY_MAX]; /* the ready queue's bytes, a ring */
    uint32_t rhead, rlen;         /* its first byte, and how many */
    uint16_t recs[LD_RECS_MAX];   /* each record's length, a ring */
    uint32_t chead, ncount;       /* the first record, and how many */
};

/* Linux's defaults for a new terminal: ICRNL and IUTF8; OPOST and ONLCR;
 * ISIG, ICANON, ECHO, ECHOE, ECHOK, ECHOCTL, ECHOKE and IEXTEN; Ctrl+C
 * INTR, Ctrl+\ QUIT, DEL ERASE, Ctrl+U KILL, Ctrl+D EOF, Ctrl+Z SUSP,
 * Ctrl+R REPRINT, Ctrl+W WERASE, Ctrl+V LNEXT, MIN 1, TIME 0. */
void ld_default(struct ld_termios *t);
/* A new line discipline with the defaults, its echo to echo(ctx, ...). */
void ld_init(struct ldisc *l, ld_echo_fn *echo, void *ctx);
/* New settings. Canonical mode turned off makes the line being edited
 * ready, as raw input (Linux does the same). */
void ld_set(struct ldisc *l, const struct ld_termios *t);

/* One byte typed. Its echo goes to the callback before this returns;
 * LD_SIG_* when ISIG's key was typed (the byte itself is not input). */
enum ld_sig ld_input(struct ldisc *l, uint8_t b);
/* Throw away what was typed: the line being edited and the ready queue. */
void ld_flush_input(struct ldisc *l);

/* Records waiting for a reader. */
uint32_t ld_ready(const struct ldisc *l);
/* The first record copied into buf (cap >= LD_REC_MAX, or it is cut): its
 * length (0: end of file), or -1 when there is none. The record stays
 * until ld_pop; peeking closes it, so nothing joins it afterwards. */
long ld_peek(struct ldisc *l, uint8_t *buf, size_t cap);
void ld_pop(struct ldisc *l);
/* Put n bytes back at the front of the ready queue as one record (input
 * a reader left unread, for the next one). false: no room (dropped). */
bool ld_unread(struct ldisc *l, const uint8_t *p, size_t n);
/* The line being edited, copied to buf (cap bytes at most) and emptied:
 * its length. For handing what was typed to a reader that takes keys. */
size_t ld_take_line(struct ldisc *l, uint8_t *buf, size_t cap);

/* A program's output, n bytes of in, as the terminal is to get it, into
 * out (2 * n bytes of room): its length. Moves the column. */
size_t ld_output(struct ldisc *l, const uint8_t *in, size_t n, uint8_t *out);

/* When a read of `want` bytes (> 0) that started at `start` and has `got`
 * bytes so far, the last of them at `last`, ends (POSIX's "Canonical Mode
 * Input Processing" and the four cases of MIN and TIME): 0 now, LD_NEVER
 * only once more input comes, or the time (the read's clock's) at which
 * it ends with what it has. Canonical mode: as soon as it has any. A
 * non-blocking read ends at once whatever this says. */
uint64_t ld_read_deadline(const struct ld_termios *t, size_t got, size_t want, uint64_t start,
                          uint64_t last);
