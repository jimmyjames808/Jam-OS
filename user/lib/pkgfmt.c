/* The ports formats' common rules (<pkgfmt.h>): the text cursor, the
 * field grammars, the signed-text prologue (size, first line, bytes,
 * signature line, Ed25519 signature: checked before any field is read),
 * the extension lines, the serial's replay rule, the version order and
 * the path rules. Monocypher's crypto_ed25519_check is the very code
 * tools/jamos-sign signs with on the Mac (as updsig.c's); a program that
 * links this file links it in. */
#include <monocypher-ed25519.h>
#include "pkgint.h"

static bool is_lower(uint8_t c)
{
    return c >= 'a' && c <= 'z';
}

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_alpha(uint8_t c)
{
    return is_lower(c) || (c >= 'A' && c <= 'Z');
}

static bool is_hex(uint8_t c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f');
}

static bool is_print(uint8_t c)
{
    return c >= 0x20 && c <= 0x7e;
}

bool pkg_str_eq(struct pkg_str a, const char *s)
{
    size_t n = strlen(s);
    return a.n == n && !memcmp(a.p, s, n);
}

int pkg_str_cmp(struct pkg_str a, struct pkg_str b)
{
    uint32_t m = a.n < b.n ? a.n : b.n;
    int c = m ? memcmp(a.p, b.p, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

/* ---- lines and words ------------------------------------------------------------------- */

bool pkg_peek_line(const struct pkg_lines *ls, struct pkg_str *line)
{
    if (ls->p >= ls->end)
        return false;
    const char *e = ls->p;
    while (e < ls->end && *e != '\n')
        e++;
    line->p = ls->p;
    line->n = (uint32_t)(e - ls->p);
    return true;
}

bool pkg_next_line(struct pkg_lines *ls, struct pkg_str *line)
{
    if (!pkg_peek_line(ls, line))
        return false;
    ls->p += line->n + 1;   /* every line ends in '\n', checked by pkg_signed_text */
    return true;
}

bool pkg_line_is(struct pkg_str line, const char *key)
{
    size_t k = strlen(key);
    return line.n >= k && !memcmp(line.p, key, k) && (line.n == k || line.p[k] == ' ');
}

/* The next field of line from *at (up to a space or the end): false if it
 * is empty. *at moves past the space after it. */
static bool field_at(struct pkg_str line, uint32_t *at, struct pkg_str *f)
{
    uint32_t e = *at;
    while (e < line.n && line.p[e] != ' ')
        e++;
    if (e == *at)
        return false;
    f->p = line.p + *at;
    f->n = e - *at;
    *at = e < line.n ? e + 1 : e + 0;
    return true;
}

bool pkg_words(struct pkg_str line, const char *key, struct pkg_str *w, unsigned lo, unsigned hi,
               unsigned *n)
{
    struct pkg_str f;
    uint32_t at = 0;
    if (!field_at(line, &at, &f) || !pkg_str_eq(f, key))
        return false;
    unsigned k = 0;
    while (at < line.n) {
        if (k == hi || !field_at(line, &at, &w[k]))
            return false;
        k++;
    }
    /* a trailing space leaves at == n after a field: the next field is empty */
    if (line.n && line.p[line.n - 1] == ' ')
        return false;
    if (k < lo)
        return false;
    if (n)
        *n = k;
    return true;
}

bool pkg_rest(struct pkg_str line, const char *key, struct pkg_str *rest)
{
    size_t k = strlen(key);
    if (line.n <= k + 1 || memcmp(line.p, key, k) || line.p[k] != ' ')
        return false;
    rest->p = line.p + k + 1;
    rest->n = line.n - (uint32_t)k - 1;
    return true;
}

bool pkg_split_rest(struct pkg_str line, const char *key, struct pkg_str *w, unsigned nw)
{
    struct pkg_str f, r;
    uint32_t at = 0;
    if (!field_at(line, &at, &f) || !pkg_str_eq(f, key))
        return false;
    for (unsigned i = 0; i + 1 < nw; i++)
        if (at >= line.n || !field_at(line, &at, &w[i]))
            return false;
    if (at >= line.n)
        return false;
    r.p = line.p + at;
    r.n = line.n - at;
    w[nw - 1] = r;
    return true;
}

/* ---- fields ---------------------------------------------------------------------------- */

static bool all_of(struct pkg_str s, uint32_t from, bool (*ok)(uint8_t))
{
    for (uint32_t i = from; i < s.n; i++)
        if (!ok((uint8_t)s.p[i]))
            return false;
    return true;
}

static bool name_rest(uint8_t c)
{
    return is_lower(c) || is_digit(c) || c == '+' || c == '.' || c == '_' || c == '-';
}

static bool version_rest(uint8_t c)
{
    return is_alpha(c) || is_digit(c) || c == '.' || c == '_' || c == '+' || c == '~' || c == '-';
}

bool pkg_f_name(struct pkg_str s)
{
    return s.n >= 1 && s.n <= 63 && (is_lower((uint8_t)s.p[0]) || is_digit((uint8_t)s.p[0])) &&
           all_of(s, 1, name_rest);
}

bool pkg_f_version(struct pkg_str s)
{
    return s.n >= 1 && s.n <= 63 && (is_alpha((uint8_t)s.p[0]) || is_digit((uint8_t)s.p[0])) &&
           all_of(s, 1, version_rest);
}

/* Decimal digits of s (1..19) with no leading zero unless it is "0" alone. */
static bool decimal(struct pkg_str s, uint64_t *out)
{
    if (s.n < 1 || s.n > 19 || (s.n > 1 && s.p[0] == '0'))
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        if (!is_digit((uint8_t)s.p[i]))
            return false;
        v = v * 10 + (uint64_t)(s.p[i] - '0');   /* 19 digits can't wrap 64 bits */
    }
    *out = v;
    return true;
}

bool pkg_f_revision(struct pkg_str s, uint32_t *out)
{
    uint64_t v;
    if (s.n < 1 || s.n > 9 || s.p[0] == '0' || !decimal(s, &v))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool pkg_f_size(struct pkg_str s, uint64_t max, uint64_t *out)
{
    uint64_t v;
    if (!decimal(s, &v) || v > max)
        return false;
    *out = v;
    return true;
}

bool pkg_hex(struct pkg_str s, uint8_t *out, size_t n)
{
    if (s.n != 2 * n)
        return false;
    for (size_t i = 0; i < s.n; i++)
        if (!is_hex((uint8_t)s.p[i]))
            return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)s.p[2 * i], lo = (uint8_t)s.p[2 * i + 1];
        out[i] = (uint8_t)(((is_digit(hi) ? hi - '0' : hi - 'a' + 10) << 4) |
                           (is_digit(lo) ? lo - '0' : lo - 'a' + 10));
    }
    return true;
}

void pkg_put_bytes(struct pkg_out *o, const char *s, size_t n)
{
    if (o->n < o->cap && n <= o->cap - o->n)
        memcpy(o->p + o->n, s, n);
    o->n += n;
}

void pkg_put_str(struct pkg_out *o, const char *s)
{
    pkg_put_bytes(o, s, strlen(s));
}

void pkg_put_uint(struct pkg_out *o, uint32_t v)
{
    char tmp[10];
    unsigned k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        pkg_put_bytes(o, &tmp[--k], 1);
}

void pkg_put_hex(struct pkg_out *o, const uint8_t *b, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char hex[2] = { digits[b[i] >> 4], digits[b[i] & 15] };
        pkg_put_bytes(o, hex, 2);
    }
}

bool pkg_f_sha(struct pkg_str s, uint8_t out[SHA256_BYTES])
{
    return pkg_hex(s, out, SHA256_BYTES);
}

bool pkg_f_abi(struct pkg_str s, char out[17])
{
    uint8_t b[8];
    if (!pkg_hex(s, b, sizeof(b)))
        return false;
    memcpy(out, s.p, 16);
    out[16] = '\0';
    return true;
}

bool pkg_f_serial(struct pkg_str s, uint64_t *out)
{
    uint64_t v;
    if (s.n < 1 || s.p[0] == '0' || !decimal(s, &v) || v > (uint64_t)INT64_MAX)
        return false;
    *out = v;
    return true;
}

/* YYYY-MM-DDTHH:MM:SSZ: digits and punctuation in their places. */
bool pkg_f_made(struct pkg_str s, char out[21])
{
    static const char shape[] = "dddd-dd-ddTdd:dd:ddZ";
    if (s.n != 20)
        return false;
    for (uint32_t i = 0; i < 20; i++) {
        bool ok = shape[i] == 'd' ? is_digit((uint8_t)s.p[i]) : s.p[i] == shape[i];
        if (!ok)
            return false;
    }
    memcpy(out, s.p, 20);
    out[20] = '\0';
    return true;
}

static bool licence_char(uint8_t c)
{
    return is_alpha(c) || is_digit(c) || c == '.' || c == '+' || c == '(' || c == ')' || c == '-';
}

/* Words of licence characters split by single spaces. */
bool pkg_f_licence(struct pkg_str s)
{
    if (s.n < 1 || s.n > PKG_TEXT_MAX)
        return false;
    bool word = false;
    for (uint32_t i = 0; i < s.n; i++) {
        if (s.p[i] == ' ') {
            if (!word)
                return false;
            word = false;
        } else if (licence_char((uint8_t)s.p[i])) {
            word = true;
        } else {
            return false;
        }
    }
    return word;
}

bool pkg_f_text(struct pkg_str s)
{
    return s.n >= 1 && s.n <= PKG_TEXT_MAX && all_of(s, 0, is_print) && s.p[0] != ' ' &&
           s.p[s.n - 1] != ' ';
}

static bool id_rest(uint8_t c)
{
    return is_alpha(c) || is_digit(c) || c == '-';
}

bool pkg_f_id(struct pkg_str s)
{
    return s.n >= 1 && s.n <= 63 && is_alpha((uint8_t)s.p[0]) && all_of(s, 1, id_rest);
}

/* ---- the signed text ------------------------------------------------------------------- */

status_t pkg_signed_text(const void *text, size_t len, const char *first, size_t max,
                         const uint8_t *key, struct pkg_lines *ls, uint8_t sha[SHA256_BYTES],
                         const char **why)
{
    const uint8_t *t = text;
    size_t flen = strlen(first);
    if (!t || len == 0 || len > max)
        return pkg_fail(why, ERR_OUT_OF_RANGE, "the text is empty or over its size bound");
    if (len < flen + 1 || memcmp(t, first, flen) || t[flen] != '\n') {
        const char *sp = first;
        while (*sp != ' ')
            sp++;
        size_t word = (size_t)(sp - first) + 1;   /* "jamos-pkg " */
        bool other = len >= word && !memcmp(t, first, word);
        return pkg_fail(why, other ? ERR_NOT_SUPPORTED : ERR_INVALID_ARGS,
                        "the first line is not this format's");
    }
    for (size_t i = 0; i < len; i++)
        if (t[i] != '\n' && !is_print(t[i]))
            return pkg_fail(why, ERR_INVALID_ARGS, "a byte that is not printable ASCII");
    if (t[len - 1] != '\n')
        return pkg_fail(why, ERR_INVALID_ARGS, "the last line has no newline");
    size_t sig = len - 1;
    while (sig > 0 && t[sig - 1] != '\n')
        sig--;
    if (sig < flen + 1)
        return pkg_fail(why, ERR_INVALID_ARGS, "no signature line");
    size_t slen = len - 1 - sig;
    uint8_t signature[PKG_SIG_BYTES];
    bool bare = slen == 9 && !memcmp(t + sig, "signature", 9);
    bool full = slen == 10 + 2 * PKG_SIG_BYTES && !memcmp(t + sig, "signature ", 10) &&
                pkg_hex((struct pkg_str){ (const char *)t + sig + 10, 2 * PKG_SIG_BYTES },
                        signature, PKG_SIG_BYTES);
    if (key && bare)
        return pkg_fail(why, ERR_ACCESS_DENIED, "unsigned: the signature line is bare");
    if (!full && !(bare && !key))
        return pkg_fail(why, ERR_INVALID_ARGS, "no 'signature <128 hex digits>' last line");
    /* 0 only for a valid signature of exactly these bytes by this key, and a
     * canonical one (S below the group order). */
    if (key && crypto_ed25519_check(signature, key, t, sig))
        return pkg_fail(why, ERR_ACCESS_DENIED,
                        "the signature is not the key's (or the text changed)");
    sha256(t, len, sha);
    ls->p = (const char *)t + flen + 1;
    ls->end = (const char *)t + sig;
    return OK;
}

/* ---- extension lines and the serial ------------------------------------------------------ */

/* An extension line's key: 1..32 bytes of a-z 0-9 -, the first not a '-';
 * a '!' (must-understand) in front counts in the 32. */
static bool ext_key_ok(struct pkg_str k)
{
    uint32_t from = k.n && k.p[0] == '!' ? 1u : 0u;
    if (k.n - from < 1 || k.n - from > 32 - from)
        return false;
    if (!is_lower((uint8_t)k.p[from]) && !is_digit((uint8_t)k.p[from]))
        return false;
    for (uint32_t i = from; i < k.n; i++)
        if (!is_lower((uint8_t)k.p[i]) && !is_digit((uint8_t)k.p[i]) && k.p[i] != '-')
            return false;
    return true;
}

status_t pkg_extensions(struct pkg_lines *ls, const char *const *known, const char **why)
{
    struct pkg_str line;
    while (pkg_peek_line(ls, &line)) {
        uint32_t e = 0;
        while (e < line.n && line.p[e] != ' ')
            e++;
        struct pkg_str key = { line.p, e };
        for (const char *const *k = known; *k; k++)
            if (pkg_str_eq(key, *k))
                return OK;
        if (!ext_key_ok(key))
            return OK;
        if (key.p[0] == '!')
            return pkg_fail(why, ERR_NOT_SUPPORTED,
                            "a must-understand line this reader does not know");
        if (e < line.n) {
            struct pkg_str value = { line.p + e + 1, line.n - e - 1 };
            if (!pkg_f_text(value))
                return pkg_fail(why, ERR_INVALID_ARGS, "an extension line's value");
        }
        pkg_next_line(ls, &line);
    }
    return OK;
}

status_t pkg_serial_line(struct pkg_str line, const struct pkg_last *last,
                         const uint8_t sha[SHA256_BYTES], uint64_t *serial, const char **why)
{
    struct pkg_str w[1];
    uint64_t s;
    NEED(pkg_words(line, "serial", w, 1, 1, NULL) && pkg_f_serial(w[0], &s), "the serial line");
    if (last && !(s > last->serial || (s == last->serial && !memcmp(sha, last->sha256, SHA256_BYTES))))
        return pkg_fail(why, ERR_BAD_STATE,
                        "an old serial replayed, or another text with the same serial");
    *serial = s;
    return OK;
}

/* ---- the version order -------------------------------------------------------------------- */

/* The next run of digits (type 0) or letters (type 1) of v from *at;
 * false at the end. Other bytes only separate. */
static bool next_run(struct pkg_str v, uint32_t *at, int *type, struct pkg_str *run)
{
    uint32_t i = *at;
    while (i < v.n && !is_digit((uint8_t)v.p[i]) && !is_alpha((uint8_t)v.p[i]))
        i++;
    if (i >= v.n)
        return false;
    *type = is_digit((uint8_t)v.p[i]) ? 0 : 1;
    uint32_t s = i;
    while (i < v.n && (*type == 0 ? is_digit((uint8_t)v.p[i]) : is_alpha((uint8_t)v.p[i])))
        i++;
    run->p = v.p + s;
    run->n = i - s;
    *at = i;
    return true;
}

static int cmp_run(int type, struct pkg_str a, struct pkg_str b)
{
    if (type == 0) {   /* numbers: leading zeros ignored, then by length, then bytewise */
        while (a.n && *a.p == '0')
            a.p++, a.n--;
        while (b.n && *b.p == '0')
            b.p++, b.n--;
        if (a.n != b.n)
            return a.n < b.n ? -1 : 1;
    }
    return pkg_str_cmp(a, b);
}

int pkg_version_cmp(struct pkg_str a, struct pkg_str b)
{
    uint32_t ia = 0, ib = 0;
    for (;;) {
        int ta = 0, tb = 0;
        struct pkg_str ra, rb;
        bool ha = next_run(a, &ia, &ta, &ra), hb = next_run(b, &ib, &tb, &rb);
        if (!ha || !hb)
            return ha ? 1 : hb ? -1 : 0;
        if (ta != tb)
            return ta < tb ? -1 : 1;
        int c = cmp_run(ta, ra, rb);
        if (c)
            return c;
    }
}

/* ---- paths ---------------------------------------------------------------------------------- */

static bool path_byte_bad(uint8_t c)
{
    return c <= 0x20 || c > 0x7e || c == '"' || c == '*' || c == ':' || c == '<' || c == '>' ||
           c == '?' || c == '\\' || c == '|';
}

const char *pkg_path_problem(struct pkg_str p)
{
    if (p.n < 1 || p.n > PKG_PATH_MAX)
        return "a path of 0 or over 255 bytes";
    for (uint32_t i = 0; i < p.n; i++)
        if (path_byte_bad((uint8_t)p.p[i]))
            return "a byte a path can't hold";
    uint32_t start = 0;
    for (uint32_t i = 0; i <= p.n; i++) {
        if (i < p.n && p.p[i] != '/')
            continue;
        uint32_t len = i - start;
        const char *part = p.p + start;
        if (len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.'))
            return "an empty, '.' or '..' part";
        if (part[len - 1] == '.')
            return "a part ending in '.' (FAT32 drops it)";
        start = i + 1;
    }
    if (p.p[0] == '+')
        return "a first part starting with '+' (the archive's own names)";
    return NULL;
}

static uint8_t lower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c;
}

bool pkg_path_fold_equal(struct pkg_str a, struct pkg_str b)
{
    if (a.n != b.n)
        return false;
    for (uint32_t i = 0; i < a.n; i++)
        if (lower((uint8_t)a.p[i]) != lower((uint8_t)b.p[i]))
            return false;
    return true;
}
