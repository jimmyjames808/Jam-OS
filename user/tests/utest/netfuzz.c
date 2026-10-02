/* utest: what the network parsers' tests share (dhcp.c, dns.c and the
 * state machines' tests): a buffer whose end touches a page nothing may
 * read, so a parser that reads one byte past a datagram faults at once
 * instead of reading stale bytes and passing; a small seeded random
 * generator (a test's random numbers are the same on every run); and
 * mutations of a valid datagram for the fuzz loops. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "nettest.h"
#include "utest.h"

bool guard_open(struct guard *g)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), vmo;
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(2 * PAGE_SIZE, 0, HANDLE_INVALID, &vmo), OK);
    status_t st = jam_vmar_map(vmar, vmo, 0, 2 * PAGE_SIZE, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    CHECK_ST(st, OK);
    /* the second page: no access at all */
    CHECK_ST(jam_vmar_protect(vmar, addr + PAGE_SIZE, PAGE_SIZE, 0), OK);
    g->base = (uint8_t *)(uintptr_t)addr;
    return true;
}

void guard_close(struct guard *g)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)g->base,
                         2 * PAGE_SIZE);   /* a test's own mapping: nothing to do if it fails */
    g->base = NULL;
}

uint8_t *guard_put(struct guard *g, const void *bytes, size_t len)
{
    if (len > PAGE_SIZE)
        len = PAGE_SIZE;
    uint8_t *at = g->base + PAGE_SIZE - len;
    memcpy(at, bytes, len);
    return at;
}

uint32_t fuzz_rand(uint64_t *s)
{
    /* xorshift64*: good enough to pick bytes and lengths */
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return (uint32_t)((*s * 0x2545f4914f6cdd1dull) >> 32);
}

/* One mutation of buf (len bytes, room for cap): the new length. */
static size_t mutate_once(uint64_t *s, uint8_t *buf, size_t len, size_t cap)
{
    static const uint8_t special[] = { 0x00, 0xff, 0xc0, 0x3f, 0x40, 0x7f, 0x80, 0x01 };
    uint32_t r = fuzz_rand(s);
    size_t at = len ? fuzz_rand(s) % len : 0;
    switch (r % 7) {
    case 0:   /* flip a bit */
        if (len)
            buf[at] ^= (uint8_t)(1u << (fuzz_rand(s) % 8));
        return len;
    case 1:   /* a byte that means something to a parser */
        if (len)
            buf[at] = special[fuzz_rand(s) % sizeof(special)];
        return len;
    case 2:   /* any byte */
        if (len)
            buf[at] = (uint8_t)fuzz_rand(s);
        return len;
    case 3:   /* cut short */
        return len ? fuzz_rand(s) % len : 0;
    case 4:   /* a compression pointer (DNS) or a length (DHCP) anywhere */
        if (len >= 2 && at + 1 < len) {
            buf[at] = (uint8_t)(0xc0 | (fuzz_rand(s) & 0x3f));
            buf[at + 1] = (uint8_t)fuzz_rand(s);
        }
        return len;
    case 5:   /* a big 16-bit count or length */
        if (at + 1 < len) {
            buf[at] = (uint8_t)(0x80 | fuzz_rand(s));
            buf[at + 1] = (uint8_t)fuzz_rand(s);
        }
        return len;
    default:  /* grow: random bytes on the end */
        for (unsigned n = fuzz_rand(s) % 16; n && len < cap; n--)
            buf[len++] = (uint8_t)fuzz_rand(s);
        return len;
    }
}

size_t fuzz_mutate(uint64_t *s, uint8_t *buf, size_t len, size_t cap)
{
    for (unsigned n = 1 + fuzz_rand(s) % 4; n; n--)
        len = mutate_once(s, buf, len, cap);
    return len;
}
