/* printf over the debug_write system call. Each printf call is formatted
 * into one buffer and written with one system call, so lines from
 * different threads or processes don't interleave mid-line (up to the
 * buffer size).
 *
 * With an SR_STDOUT channel in the startup message (the shell gives one to
 * a program whose output goes into a pipe: `run prog | grep x`), the
 * bytes go there instead, one message per printf; if the reader has gone,
 * back to debug_write. */
#include <stdbool.h>
#include <os.h>

#define PRINTF_BUF 512

struct out {
    char  *buf;       /* the caller's buffer */
    size_t size;      /* its size, the NUL included */
    size_t len;   /* chars that would have been written */
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, bool upper, bool neg, int width,
                    char pad, bool left, const char *prefix)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    int plen = (int)strlen(prefix) + (neg ? 1 : 0);
    int total = n + plen;
    if (!left && pad == ' ')
        for (; width > total; width--)
            put(o, ' ');
    if (neg)
        put(o, '-');
    for (; *prefix; prefix++)
        put(o, *prefix);
    if (!left && pad == '0')
        for (; width > total; width--)
            put(o, '0');
    while (n)
        put(o, tmp[--n]);
    if (left)
        for (; width > total; width--)
            put(o, ' ');
}

/* What comes between a '%' and its conversion character. */
struct spec {
    char pad;         /* ' ' or '0' (the 0 flag) */
    bool left, alt;   /* the - flag: pad on the right; the # flag: 0x / 0X */
    int  width;       /* the minimum field width */
    int  prec;        /* the precision, -1: none (only %s uses it) */
    int  lng;         /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
};

/* Parse the flags, width, precision and length at *fmt (just after the
 * '%'); *fmt is left at the conversion character. */
static void parse_spec(const char **fmt, va_list *ap, struct spec *s)
{
    const char *f = *fmt;
    s->pad = ' ';
    s->left = s->alt = false;
    for (;; f++) {
        if (*f == '-')
            s->left = true;
        else if (*f == '0')
            s->pad = '0';
        else if (*f == '#')
            s->alt = true;
        else
            break;
    }
    if (s->left)
        s->pad = ' ';
    s->width = 0;
    if (*f == '*') {
        s->width = va_arg(*ap, int);
        if (s->width < 0) {
            s->left = true;
            s->pad = ' ';
            s->width = -s->width;
        }
        f++;
    } else {
        while (*f >= '0' && *f <= '9')
            s->width = s->width * 10 + (*f++ - '0');
    }
    s->prec = -1;
    if (*f == '.') {
        f++;
        s->prec = 0;
        if (*f == '*') {
            s->prec = va_arg(*ap, int);
            f++;
        } else {
            while (*f >= '0' && *f <= '9')
                s->prec = s->prec * 10 + (*f++ - '0');
        }
    }
    s->lng = 0;
    if (*f == 'l') {
        s->lng = 1;
        if (*++f == 'l') {
            s->lng = 2;
            f++;
        }
    } else if (*f == 'z') {
        s->lng = 3;
        f++;
    }
    *fmt = f;
}

/* %s with a width and precision. */
static void put_str(struct out *o, const char *s, const struct spec *sp)
{
    if (!s)
        s = "(null)";
    int len = (int)(sp->prec >= 0 ? strnlen(s, (size_t)sp->prec) : strlen(s));
    int width = sp->width;
    if (!sp->left)
        for (; width > len; width--)
            put(o, ' ');
    for (int i = 0; i < len; i++)
        put(o, s[i]);
    if (sp->left)
        for (; width > len; width--)
            put(o, ' ');
}

/* The conversion at *fmt, its argument from ap. A '%' at the end of the
 * format leaves *fmt on the NUL's left, so the caller's loop ends. */
static void convert(struct out *o, const char **fmt, const struct spec *s, va_list *ap)
{
    char c = **fmt;
    switch (c) {
    case 'd':
    case 'i': {
        int64_t v = s->lng == 0 ? va_arg(*ap, int)
                  : s->lng == 3 ? (int64_t)va_arg(*ap, size_t)
                                : va_arg(*ap, long long);
        bool neg = v < 0;
        put_num(o, neg ? -(uint64_t)v : (uint64_t)v, 10, false, neg, s->width, s->pad, s->left,
                "");
        break;
    }
    case 'u':
    case 'x':
    case 'X': {
        uint64_t v = s->lng == 0 ? va_arg(*ap, unsigned)
                   : s->lng == 3 ? va_arg(*ap, size_t)
                                 : va_arg(*ap, unsigned long long);
        const char *prefix = s->alt && c != 'u' ? (c == 'X' ? "0X" : "0x") : "";
        put_num(o, v, c == 'u' ? 10 : 16, c == 'X', false, s->width, s->pad, s->left, prefix);
        break;
    }
    case 'p':
        put_num(o, (uintptr_t)va_arg(*ap, void *), 16, false, false, 0, ' ', false, "0x");
        break;
    case 's':
        put_str(o, va_arg(*ap, const char *), s);
        break;
    case 'c':
        put(o, (char)va_arg(*ap, int));
        break;
    case '%':
        put(o, '%');
        break;
    case '\0':
        (*fmt)--;
        break;
    default:
        put(o, '%');
        put(o, c);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    va_list aq;   /* a copy the helpers can take the address of */
    va_copy(aq, ap);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        struct spec s;
        parse_spec(&fmt, &aq, &s);
        convert(&o, &fmt, &s, &aq);
    }
    va_end(aq);
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static void emit(const char *s, uint64_t n)
{
    static int have = -1;   /* SR_STDOUT: -1 not looked yet, 0 none/gone, 1 yes */
    static handle_t out;
    if (have < 0) {
        out = startup_handle(SR_STDOUT);
        have = out != HANDLE_INVALID;
    }
    while (have > 0 && n) {
        uint32_t k = n > 4096 ? 4096 : (uint32_t)n;
        status_t st = jam_channel_write(out, s, k, NULL, 0);
        if (st == ERR_SHOULD_WAIT) {   /* the reader is behind: wait for room */
            signals_t seen;
            jam_object_wait_one(out, SIG_WRITABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
            continue;
        }
        if (st != OK) {
            have = 0;   /* gone: the log from now on */
            break;
        }
        s += k;
        n -= k;
    }
    if (n)
        jam_debug_write(s, n);
}

int vprintf(const char *fmt, va_list ap)
{
    char buf[PRINTF_BUF];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return n;
    }
    if ((size_t)n < sizeof(buf)) {
        emit(buf, (uint64_t)n);
    } else {
        /* Too long for the stack buffer: format again into the heap, or
         * print the truncated line if even that fails. */
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            emit(big, (uint64_t)n);
            free(big);
        } else {
            emit(buf, sizeof(buf) - 1);
        }
    }
    va_end(ap2);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    return printf("%s\n", s);
}

const char *status_str(status_t s)
{
    switch (s) {
    case OK:                   return "OK";
    case ERR_INTERNAL:         return "ERR_INTERNAL";
    case ERR_NOT_SUPPORTED:    return "ERR_NOT_SUPPORTED";
    case ERR_NO_MEMORY:        return "ERR_NO_MEMORY";
    case ERR_INVALID_ARGS:     return "ERR_INVALID_ARGS";
    case ERR_BAD_HANDLE:       return "ERR_BAD_HANDLE";
    case ERR_WRONG_TYPE:       return "ERR_WRONG_TYPE";
    case ERR_ACCESS_DENIED:    return "ERR_ACCESS_DENIED";
    case ERR_BAD_STATE:        return "ERR_BAD_STATE";
    case ERR_OUT_OF_RANGE:     return "ERR_OUT_OF_RANGE";
    case ERR_BUFFER_TOO_SMALL: return "ERR_BUFFER_TOO_SMALL";
    case ERR_SHOULD_WAIT:      return "ERR_SHOULD_WAIT";
    case ERR_TIMED_OUT:        return "ERR_TIMED_OUT";
    case ERR_PEER_CLOSED:      return "ERR_PEER_CLOSED";
    case ERR_CANCELED:         return "ERR_CANCELED";
    case ERR_ALREADY_BOUND:    return "ERR_ALREADY_BOUND";
    case ERR_NOT_FOUND:        return "ERR_NOT_FOUND";
    case ERR_NO_RESOURCES:     return "ERR_NO_RESOURCES";
    }
    return "?";
}
