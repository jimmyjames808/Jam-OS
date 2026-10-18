/* A stand-in C library for `make check`'s link test of libjam.a
 * (tools/checklibjam.py): the functions libjam calls by their C library
 * names, which a POSIX program gets from musl. It is linked with every
 * object of libjam (--whole-archive, --fatal-warnings), so the link fails
 * if libjam needs a C library function this file doesn't have (add it
 * here, and only a standard name: musl must have it too) or defines one
 * itself (it would clash with musl's). Besides those, libjam's allocator
 * (jam_malloc, jam_calloc, jam_free: <os.h>), which a POSIX program gets
 * from the call layer's heap: libjam never calls malloc, calloc or free,
 * and the link fails if it does. Nothing here ever runs: each function
 * only returns. */
#include <stdarg.h>
#include <stddef.h>

void  *jam_malloc(size_t n);
void  *jam_calloc(size_t n, size_t size);
void   jam_free(void *p);
void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
int    printf(const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
void   _start(void);

void *jam_malloc(size_t n)
{
    (void)n;
    return NULL;
}

void *jam_calloc(size_t n, size_t size)
{
    (void)n, (void)size;
    return NULL;
}

void jam_free(void *p)
{
    (void)p;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    (void)src, (void)n;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    (void)src, (void)n;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    (void)c, (void)n;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    (void)a, (void)b, (void)n;
    return 0;
}

size_t strlen(const char *s)
{
    (void)s;
    return 0;
}

size_t strnlen(const char *s, size_t max)
{
    (void)s, (void)max;
    return 0;
}

int strcmp(const char *a, const char *b)
{
    (void)a, (void)b;
    return 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
    (void)a, (void)b, (void)n;
    return 0;
}

char *strchr(const char *s, int c)
{
    (void)s, (void)c;
    return NULL;
}

char *strrchr(const char *s, int c)
{
    (void)s, (void)c;
    return NULL;
}

char *strstr(const char *hay, const char *needle)
{
    (void)hay, (void)needle;
    return NULL;
}

int printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    (void)buf, (void)size, (void)fmt;
    return 0;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    (void)buf, (void)size, (void)fmt, (void)ap;
    return 0;
}

/* The entry the link needs (a POSIX program's is the POSIX crt's). */
void _start(void)
{
    for (;;)
        ;
}
