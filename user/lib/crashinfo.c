/* A saved panic's text, read (<crashinfo.h>). Every search is bounded by
 * the bytes given; nothing is trusted to be terminated. */
#include <crashinfo.h>
#include <os.h>

/* The first place p (len bytes) is in s[0..n), or NULL. */
static const char *find(const char *s, size_t n, const char *p)
{
    size_t len = strlen(p);
    for (size_t i = 0; len <= n && i <= n - len; i++)
        if (!memcmp(s + i, p, len))
            return s + i;
    return NULL;
}

/* The end of the line at p (its '\n', or s + n). */
static const char *line_end(const char *p, const char *end)
{
    while (p < end && *p != '\n')
        p++;
    return p;
}

/* [from, to) into out (size bytes), cut, every byte outside printable
 * ASCII as '?'. */
static void copy_text(char *out, size_t size, const char *from, const char *to)
{
    size_t k = 0;
    for (; from < to && k + 1 < size; from++)
        out[k++] = *from >= 0x20 && *from < 0x7f ? *from : '?';
    out[k] = 0;
}

static bool is_code(const char *p, const char *end, size_t *len)
{
    if (end - p < 11 || memcmp(p, "JAM-", 4))
        return false;
    const char *q = p + 4;
    int caps = 0;
    while (q < end && *q >= 'A' && *q <= 'Z' && caps < 4) {
        q++;
        caps++;
    }
    if (caps < 2 || caps > 3 || end - q < 5 || *q != '-')
        return false;
    for (int i = 1; i <= 4; i++) {
        char c = q[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
    }
    q += 5;
    if (q < end && ((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z')))
        return false;   /* longer than a code */
    *len = (size_t)(q - p);
    return true;
}

/* The first good code's place (after its "code "), into code; NULL (code
 * "") if none. */
static const char *code_at(const char *text, size_t n, char code[CRASH_CODE_MAX])
{
    code[0] = 0;
    const char *end = text + n, *p = text;
    while ((p = find(p, (size_t)(end - p), "code JAM-")) != NULL) {
        size_t len;
        if (is_code(p + 5, end, &len)) {
            memcpy(code, p + 5, len);
            code[len] = 0;
            return p + 5;
        }
        p++;
    }
    return NULL;
}

bool crash_code(const char *text, size_t n, char code[CRASH_CODE_MAX])
{
    return code_at(text, n, code) != NULL;
}

/* A decimal number at p, *v; false if there are no digits. */
static bool number(const char *p, const char *end, uint64_t *v)
{
    *v = 0;
    const char *q = p;
    for (; q < end && *q >= '0' && *q <= '9' && q - p < 19; q++)
        *v = *v * 10 + (uint64_t)(*q - '0');
    return q > p;
}

bool crash_read_intro(const char *s, size_t n, struct crash_report *r)
{
    memset(r, 0, sizeof(*r));
    const char *end = s + n;
    static const char head[] = "Jam OS crash log: the end of the kernel log of ";
    if (n < sizeof(head) - 1 || memcmp(s, head, sizeof(head) - 1))
        return false;
    const char *b = s + sizeof(head) - 1, *e = line_end(b, end);
    const char *saved = find(b, (size_t)(e - b), ", saved by");
    copy_text(r->boot, sizeof(r->boot), b, saved ? saved : e);
    const char *m = find(s, n, "\nThe panic: ");
    if (m) {
        m += 12;
        copy_text(r->message, sizeof(r->message), m, line_end(m, end));
    }
    const char *at = find(s, n, "\nThe panic starts at byte ");
    const char *dash = find(s, n, "\n--------\n");
    if (!at || !dash || !number(at + 26, end, &r->panic_at))
        return false;
    r->text_at = (uint64_t)(dash + 10 - s);
    return true;
}

void crash_read_panic(const char *s, size_t n, struct crash_report *r)
{
    const char *end = s + n, *p = code_at(s, n, r->code);
    if (p) {
        p += strlen(r->code);
        if (end - p > 2 && p[0] == ':' && p[1] == ' ') {
            const char *e = line_end(p + 2, end), *semi = find(p + 2, (size_t)(e - p - 2), ";");
            copy_text(r->what, sizeof(r->what), p + 2, semi ? semi : e);
        }
    }
    if ((p = find(s, n, "*** JAM OS KERNEL PANIC *** on ")) != NULL) {
        p += 31;
        const char *e = line_end(p, end), *paren = find(p, (size_t)(e - p), " (other");
        copy_text(r->where, sizeof(r->where), p, paren ? paren : e);
    }
    if ((p = find(s, n, "  build Jam OS ")) != NULL) {
        p += 8;
        copy_text(r->build, sizeof(r->build), p, line_end(p, end));
    }
}

/* The line at p without the log's "[  seconds] " in front and the
 * blanks after it. */
static const char *after_stamp(const char *p, const char *e)
{
    if (p < e && *p == '[') {
        const char *q = p;
        while (q < e && *q != ']')
            q++;
        if (q < e)
            p = q + 1;
    }
    while (p < e && *p == ' ')
        p++;
    return p;
}

bool crash_next_frame(const char *s, size_t n, size_t *at, char *line, size_t size)
{
    const char *end = s + n, *p;
    if (*at == 0) {
        const char *bt = find(s, n, "backtrace:\n");
        if (!bt)
            return false;
        *at = (size_t)(bt + 11 - s);
    }
    if (*at >= n)
        return false;
    p = s + *at;
    const char *e = line_end(p, end), *t = after_stamp(p, e);
    bool frame = e - t > 1 && t[0] == '#' && t[1] >= '0' && t[1] <= '9';
    if (!frame && !(e - t > 3 && !memcmp(t, "...", 3)))
        return false;
    copy_text(line, size, t, e);
    *at = (size_t)(e - s) + 1;
    return true;
}
