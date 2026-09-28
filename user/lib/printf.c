/* printf over the debug_write system call. Each printf call is formatted
 * into one buffer and written with one system call, so lines from
 * different threads or processes don't interleave mid-line (up to the
 * buffer size). */
#include <stdbool.h>
#include <os.h>

#define PRINTF_BUF 512

struct out {
    char  *buf;
    size_t size;
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

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        bool left = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else if (*fmt == '#')
                alt = true;
            else
                break;
        }
        if (left)
            pad = ' ';
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                pad = ' ';
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;   /* only used by %s */
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        int lng = 0;   /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
        if (*fmt == 'l') {
            lng = 1;
            if (*++fmt == 'l') {
                lng = 2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            lng = 3;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng == 0 ? va_arg(ap, int)
                      : lng == 3 ? (int64_t)va_arg(ap, size_t)
                                 : va_arg(ap, long long);
            bool neg = v < 0;
            put_num(&o, neg ? -(uint64_t)v : (uint64_t)v, 10, false, neg, width, pad, left, "");
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng == 0 ? va_arg(ap, unsigned)
                       : lng == 3 ? va_arg(ap, size_t)
                                  : va_arg(ap, unsigned long long);
            const char *prefix = alt && *fmt != 'u' ? (*fmt == 'X' ? "0X" : "0x") : "";
            put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, pad, left, prefix);
            break;
        }
        case 'p':
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, false, 0, ' ', false, "0x");
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            if (!left)
                for (; width > len; width--)
                    put(&o, ' ');
            for (int i = 0; i < len; i++)
                put(&o, s[i]);
            if (left)
                for (; width > len; width--)
                    put(&o, ' ');
            break;
        }
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
        }
    }

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
        jam_debug_write(buf, (uint64_t)n);
    } else {
        /* Too long for the stack buffer: format again into the heap, or
         * print the truncated line if even that fails. */
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            jam_debug_write(big, (uint64_t)n);
            free(big);
        } else {
            jam_debug_write(buf, sizeof(buf) - 1);
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
