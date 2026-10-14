/* What user/lib/pkg*.c share: the text cursor, the field grammars and the
 * signed-text prologue of the ports formats (<pkgfmt.h>). Like the
 * readers it serves it calls no allocator and no system call, so the Mac's
 * check (tools/pkgcheck.c) builds the same files. */
#pragma once

#include <os.h>
#include <pkgfmt.h>

/* The lines of a text, each ended by '\n'. */
struct pkg_lines {
    const char *p;     /* the next line's first byte */
    const char *end;   /* one past the last line's '\n' */
};

/* Fail with st and a reason; for the readers' NEED(). */
static inline status_t pkg_fail(const char **why, status_t st, const char *msg)
{
    if (why)
        *why = msg;
    return st;
}
#define NEED(cond, msg)                                         \
    do {                                                        \
        if (!(cond))                                            \
            return pkg_fail(why, ERR_INVALID_ARGS, (msg));      \
    } while (0)

/* A signed text up to its signature line: the size bound (1..max), the
 * first line, the bytes (0x20..0x7e and '\n'), a last line "signature
 * <128 hex digits>" after at least one other, and, with key, that the key
 * signed every byte before it. Without a key (the unchecked readers) a
 * bare "signature" last line passes too. *ls is the lines between the
 * first and the signature line; sha the SHA-256 of the whole text. */
status_t pkg_signed_text(const void *text, size_t len, const char *first, size_t max,
                         const uint8_t *key, struct pkg_lines *ls, uint8_t sha[SHA256_BYTES],
                         const char **why);

/* The next line without its '\n' (consumed / not consumed); false at the end. */
bool pkg_next_line(struct pkg_lines *ls, struct pkg_str *line);
bool pkg_peek_line(const struct pkg_lines *ls, struct pkg_str *line);
/* Is line's first word (up to the first space) key? */
bool pkg_line_is(struct pkg_str line, const char *key);
/* line is `key` and lo..hi single-space-separated, non-empty fields into
 * w (n of them). */
bool pkg_words(struct pkg_str line, const char *key, struct pkg_str *w, unsigned lo, unsigned hi,
               unsigned *n);
/* line is `key <rest>` with a non-empty rest. */
bool pkg_rest(struct pkg_str line, const char *key, struct pkg_str *rest);
/* line is `key` and exactly nw fields, the last of which is the rest of
 * the line (spaces and all, but not empty). */
bool pkg_split_rest(struct pkg_str line, const char *key, struct pkg_str *w, unsigned nw);


/* The fields' grammars (tools/portslib/pkgformat.py's regexes). */
bool pkg_f_name(struct pkg_str s);
bool pkg_f_version(struct pkg_str s);
bool pkg_f_revision(struct pkg_str s, uint32_t *out);
bool pkg_f_size(struct pkg_str s, uint64_t max, uint64_t *out);
bool pkg_f_sha(struct pkg_str s, uint8_t out[SHA256_BYTES]);
bool pkg_f_abi(struct pkg_str s, char out[17]);
bool pkg_f_serial(struct pkg_str s, uint64_t *out);
bool pkg_f_made(struct pkg_str s, char out[21]);
bool pkg_f_licence(struct pkg_str s);
bool pkg_f_text(struct pkg_str s);
bool pkg_f_id(struct pkg_str s);
/* Is a..b strictly increasing, a's key before b's? For checking order as
 * lines are read. */
static inline bool pkg_increasing(struct pkg_str prev, struct pkg_str next)
{
    return pkg_str_cmp(prev, next) < 0;
}

/* The extension lines at ls (a line `<key>` or `<key> <value>`, the key
 * 1..32 of a-z 0-9 - and not one of known, NULL-terminated): skipped. A
 * must-understand key ('!' first) is ERR_NOT_SUPPORTED. */
status_t pkg_extensions(struct pkg_lines *ls, const char *const *known, const char **why);
/* The `serial <n>` line, and the replay rule against last (may be NULL):
 * ERR_BAD_STATE for a serial below the last or the same with other bytes. */
status_t pkg_serial_line(struct pkg_str line, const struct pkg_last *last,
                         const uint8_t sha[SHA256_BYTES], uint64_t *serial, const char **why);
/* Text written into a buffer of cap bytes: n counts every byte asked
 * for, written or not, so n > cap says it did not fit. */
struct pkg_out {
    char  *p;
    size_t cap, n;
};
void pkg_put_bytes(struct pkg_out *o, const char *s, size_t n);
void pkg_put_str(struct pkg_out *o, const char *s);
void pkg_put_uint(struct pkg_out *o, uint32_t v);
void pkg_put_hex(struct pkg_out *o, const uint8_t *b, size_t n);
