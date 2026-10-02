/* The compiler may emit calls to memcpy/memset/memmove/memcmp even in
 * freestanding code, so these must always exist. */
#include <stdint.h>
#include <jam/string.h>

/* rep movsb / rep stosb: microcoded fast paths on every CPU since Ivy
 * Bridge ("ERMS"), and far faster than byte loops without SSE. */
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

void explicit_bzero(void *p, size_t n)
{
    memset(p, 0, n);
    /* The compiler must assume the asm reads the zeroed bytes. */
    __asm__ volatile("" : : "r"(p) : "memory");
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
