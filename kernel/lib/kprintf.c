#include <stdbool.h>
#include <stdint.h>
#include <jam/klog.h>
#include <jam/kprintf.h>

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

static void put_num(struct out *o, uint64_t v, unsigned base, bool upper,
                    bool neg, int width, char pad, bool left)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    int total = n + (neg ? 1 : 0);
    if (left) {
        if (neg)
            put(o, '-');
        while (n)
            put(o, tmp[--n]);
        for (; width > total; width--)
            put(o, ' ');
        return;
    }
    if (neg && pad == '0')
        put(o, '-');
    for (; width > total; width--)
        put(o, pad);
    if (neg && pad != '0')
        put(o, '-');
    while (n)
        put(o, tmp[--n]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        bool left = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        if (left)
            pad = ' ';
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

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
            put_num(&o, neg ? -(uint64_t)v : (uint64_t)v, 10, false, neg, width, pad, left);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng == 0 ? va_arg(ap, unsigned)
                       : lng == 3 ? va_arg(ap, size_t)
                                  : va_arg(ap, unsigned long long);
            put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, pad, left);
            break;
        }
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, false, 16, '0', false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                for (; width > len; width--)
                    put(&o, ' ');
            while (*s)
                put(&o, *s++);
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

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}
