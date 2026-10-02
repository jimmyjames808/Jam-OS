/* Random numbers for programs (<os.h>): the kernel's generator through the
 * random_get system call. Nothing is kept or generated here, so a forked
 * or restarted program can't repeat a stream. */
#include <os.h>

void os_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        size_t n = len < RANDOM_GET_MAX ? len : RANDOM_GET_MAX;
        status_t st = jam_random_get(p, n);
        if (st != OK) {
            printf("os_random: %zu bytes at %p are not writable (%s)\n", n, (void *)p,
                   status_str(st));
            __builtin_trap();
        }
        p += n;
        len -= n;
    }
}

uint32_t os_random_u32(void)
{
    uint32_t v;
    os_random(&v, sizeof(v));
    return v;
}
