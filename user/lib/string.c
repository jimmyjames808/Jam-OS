/* String and memory functions. The compiler may emit calls to
 * memcpy/memset/memmove/memcmp on its own (struct copies, zeroing), so
 * these must always exist. rep movsb / rep stosb are fast on every CPU
 * with ERMS and keep these from being "optimised" into calls to
 * themselves. */
#include <os.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    void *ret = dst;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    /* Overlapping with dst above src: copy backwards. */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}
