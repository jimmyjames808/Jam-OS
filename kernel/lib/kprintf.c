/* kvsnprintf and friends: a small printf for the kernel (the formats are
 * listed in kprintf.h). kprintf formats into a 512-byte stack buffer
 * (longer output is cut) and hands the result to klog in one call. */
#include <stdbool.h>
#include <stdint.h>
#include <jam/klog.h>
#include <jam/kprintf.h>

struct out {
    char  *buf;   /* where the output goes */
    size_t size;  /* its size, NUL included */
    size_t len;   /* chars that would have been written */
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

/* A conversion's flags, width and length modifier, as in "%-08lx". */
struct spec {
    char pad;     /* ' ' or '0' (the '0' flag; ignored with '-') */
    bool left;    /* '-': pad on the right */
    bool alt;     /* '#': 0x prefix on non-zero %x/%X values, as in C */
    int  width;   /* minimum field width */
    int  lng;     /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
};

static void put_num(struct out *o, uint64_t v, unsigned base, bool upper, bool neg,
                    const struct spec *sp)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    int total = n + (neg ? 1 : 0);
    int width = sp->width;
    char pad = sp->pad;
    if (sp->left) {
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

/* Parse the flags, width and length modifier that follow a '%'; returns
 * where the conversion character is. */
static const char *parse_spec(const char *fmt, struct spec *sp)
{
    sp->pad = ' ';
    sp->left = false;
    sp->alt = false;
    for (;; fmt++) {
        if (*fmt == '-')
            sp->left = true;
        else if (*fmt == '0')
            sp->pad = '0';
        else if (*fmt == '#')
            sp->alt = true;
        else
            break;
    }
    if (sp->left)
        sp->pad = ' ';
    sp->width = 0;
    while (*fmt >= '0' && *fmt <= '9')
        sp->width = sp->width * 10 + (*fmt++ - '0');

    sp->lng = 0;
    if (*fmt == 'l') {
        sp->lng = 1;
        if (*++fmt == 'l') {
            sp->lng = 2;
            fmt++;
        }
    } else if (*fmt == 'z') {
        sp->lng = 3;
        fmt++;
    }
    return fmt;
}

/* %u, %x and %X (conv), with %#x's prefix. */
static void put_unsigned(struct out *o, struct spec sp, char conv, uint64_t v)
{
    if (sp.alt && v && conv != 'u') {
        /* The width counts the prefix: spaces go before it, zeros
         * after it. */
        int nd = 0;
        for (uint64_t t = v; t; t >>= 4)
            nd++;
        if (!sp.left && sp.pad == ' ')
            for (int k = nd + 2; k < sp.width; k++)
                put(o, ' ');
        put(o, '0');
        put(o, conv == 'X' ? 'X' : 'x');
        sp.width = (!sp.left && sp.pad == ' ') ? 0 : (sp.width > 2 ? sp.width - 2 : 0);
    }
    put_num(o, v, conv == 'u' ? 10 : 16, conv == 'X', false, &sp);
}

static void put_str(struct out *o, const struct spec *sp, const char *s)
{
    if (!s)
        s = "(null)";
    int len = 0;
    while (s[len])
        len++;
    int width = sp->width;
    if (!sp->left)
        for (; width > len; width--)
            put(o, ' ');
    while (*s)
        put(o, *s++);
    if (sp->left)
        for (; width > len; width--)
            put(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        struct spec sp;
        fmt = parse_spec(fmt + 1, &sp);

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = sp.lng == 0 ? va_arg(ap, int)
                      : sp.lng == 3 ? (int64_t)va_arg(ap, size_t)
                                    : va_arg(ap, long long);
            bool neg = v < 0;
            put_num(&o, neg ? -(uint64_t)v : (uint64_t)v, 10, false, neg, &sp);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = sp.lng == 0 ? va_arg(ap, unsigned)
                       : sp.lng == 3 ? va_arg(ap, size_t)
                                     : va_arg(ap, unsigned long long);
            put_unsigned(&o, sp, *fmt, v);
            break;
        }
        case 'p': {
            static const struct spec ptr = { '0', false, false, 16, 0 };
            put(&o, '0');
            put(&o, 'x');
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, false, &ptr);
            break;
        }
        case 's':
            put_str(&o, &sp, va_arg(ap, const char *));
            break;
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
