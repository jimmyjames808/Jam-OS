/* utest: the named socket broker, /svc/unix (bin/unix; abi/idl/unix.idl),
 * through its protocol (posixtest's unix section has libc's side). Skipped
 * without /svc/unix.
 *
 * t_unix_names: a bound listener isn't reached until it listens; an
 * abstract name taken twice ERR_ALREADY_EXISTS; a socket file's key
 * reaches its listener, a zero key or another key nothing; listen on an
 * opener's channel and bind or connect on a listener's ERR_NOT_SUPPORTED;
 * a connection's two ends talk (the listener's from its `conn` event);
 * a listener's name is free once its channel closes.
 *
 * t_unix_backlog: listen's backlog clamped to 1 .. BACKLOG_MAX (0 takes one
 * waiting, 1000 takes BACKLOG_MAX), the next connect ERR_SHOULD_WAIT until
 * one is read; the waiting ends closed with the listener (the connector
 * sees its peer gone).
 *
 * t_unix_limits: LISTENERS_PER_OPENER on one opener, LISTENERS_MAX across
 * openers, OPENERS_MAX openers, each ERR_NO_RESOURCES past it and free
 * again once closed; an opener closed with a listener still bound through
 * it keeps its slot until that listener closes (CR-REVIEW-POSIX Q2);
 * utest's job holds the handles it held before.
 *
 * t_unix_own: libos's grants give /svc/unix by name as an opener of the
 * program's own (connect 0: one bind works, svc.connect on it gets none)
 * and "*" as our channel is (connect 1: svc.connect hands out openers). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/nschan.h>
#include <idl/svc.h>
#include <idl/unix.h>
#include <os.h>
#include "utest.h"

#define WAIT  (10 * NS_PER_S)
#define SOON  (2 * NS_PER_S)   /* the broker hears a closed channel */

static const uint8_t zero_key[UNIX_KEY_BYTES];

/* An opener's own channel to /svc/unix, or HANDLE_INVALID (skipped). */
static handle_t opener(void)
{
    handle_t h = HANDLE_INVALID;
    if (svc_open(SVC_UNIX, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

static status_t bind_abs(handle_t o, const char *name, handle_t *l)
{
    uint8_t key[UNIX_KEY_BYTES];
    return unix_bind_within(o, WAIT, (const uint8_t *)name, (uint32_t)strlen(name), l, key);
}

static status_t connect_abs(handle_t o, const char *name, handle_t *c)
{
    return unix_connect_within(o, WAIT, (const uint8_t *)name, (uint32_t)strlen(name), zero_key,
                               c);
}

static void got_conn(void *ctx, handle_t end)
{
    *(handle_t *)ctx = end;
}

/* The next connection waiting at listener l: its end, or HANDLE_INVALID. */
static handle_t take(handle_t l)
{
    static const struct unix_events ev = { .conn = got_conn };
    handle_t end = HANDLE_INVALID;
    if (jam_object_wait_one(l, SIG_READABLE, now() + WAIT, NULL) != OK)
        return HANDLE_INVALID;
    (void)unix_read_event(l, &ev, &end);
    return end;
}

/* Bind name on o until the broker has let go of an earlier holder (its
 * channel closed a moment ago): the status of the last try. */
static status_t bind_soon(handle_t o, const char *name, handle_t *l)
{
    uint64_t until = now() + SOON;
    status_t st;
    while ((st = bind_abs(o, name, l)) == ERR_ALREADY_EXISTS || st == ERR_NO_RESOURCES) {
        if (now() > until)
            break;
        (void)jam_sleep_until(now() + NS_PER_MS);
    }
    return st;
}

bool t_unix_names(void)
{
    handle_t o = opener();
    if (!o) {
        printf("utest: unix_names: skipped: no /svc/unix\n");
        return true;
    }
    handle_t l, l2, c, s;
    CHECK_ST(bind_abs(o, "utest-a", &l), OK);
    CHECK_ST(bind_abs(o, "utest-a", &l2), ERR_ALREADY_EXISTS);
    CHECK_ST(connect_abs(o, "utest-a", &c), ERR_NOT_FOUND);   /* not listening */
    CHECK_ST(unix_listen_within(o, WAIT, 4), ERR_NOT_SUPPORTED);   /* an opener's channel */
    uint8_t key[UNIX_KEY_BYTES];
    CHECK_ST(unix_bind_within(l, WAIT, NULL, 0, &l2, key), ERR_NOT_SUPPORTED);
    CHECK_ST(unix_connect_within(l, WAIT, NULL, 0, key, &c), ERR_NOT_SUPPORTED);
    CHECK_ST(unix_listen_within(l, WAIT, 4), OK);
    CHECK_ST(connect_abs(o, "utest-a", &c), OK);
    CHECK((s = take(l)) != HANDLE_INVALID);
    uint8_t b = 0x5a, got = 0;
    uint32_t n = 0;
    CHECK_ST(jam_channel_write(c, &b, 1, NULL, 0), OK);
    struct channel_read_args r = { .h = s, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)&got,
                                   .rd_actual = (uint64_t)(uintptr_t)&n };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(n == 1 && got == 0x5a);
    jam_handle_close(c);
    jam_handle_close(s);
    /* A socket file's listener: its key, and no other, reaches it. */
    handle_t f;
    uint8_t fkey[UNIX_KEY_BYTES], other[UNIX_KEY_BYTES];
    CHECK_ST(unix_bind_within(o, WAIT, NULL, 0, &f, fkey), OK);
    CHECK(memcmp(fkey, zero_key, sizeof(fkey)));
    CHECK_ST(unix_listen_within(f, WAIT, 1), OK);
    CHECK_ST(unix_connect_within(o, WAIT, NULL, 0, zero_key, &c), ERR_NOT_FOUND);
    memcpy(other, fkey, sizeof(other));
    other[7] ^= 1;
    CHECK_ST(unix_connect_within(o, WAIT, NULL, 0, other, &c), ERR_NOT_FOUND);
    CHECK_ST(unix_connect_within(o, WAIT, NULL, 0, fkey, &c), OK);
    jam_handle_close(c);
    jam_handle_close(f);
    /* Closed: the name is free again (once the broker has heard). */
    jam_handle_close(l);
    CHECK_ST(bind_soon(o, "utest-a", &l), OK);
    CHECK_ST(unix_listen_within(l, WAIT, 1), OK);
    jam_handle_close(l);
    jam_handle_close(o);
    return true;
}

bool t_unix_backlog(void)
{
    handle_t o = opener();
    if (!o) {
        printf("utest: unix_backlog: skipped: no /svc/unix\n");
        return true;
    }
    handle_t l, c[UNIX_BACKLOG_MAX + 1], extra;
    CHECK_ST(bind_soon(o, "utest-b", &l), OK);
    CHECK_ST(unix_listen_within(l, WAIT, 0), OK);   /* clamped to 1 */
    CHECK_ST(connect_abs(o, "utest-b", &c[0]), OK);
    CHECK_ST(connect_abs(o, "utest-b", &extra), ERR_SHOULD_WAIT);
    handle_t s = take(l);
    CHECK(s != HANDLE_INVALID);
    jam_handle_close(s);
    CHECK_ST(connect_abs(o, "utest-b", &c[1]), OK);
    jam_handle_close(c[0]);
    jam_handle_close(c[1]);   /* its listener's end still waits: taken below */
    CHECK((s = take(l)) != HANDLE_INVALID);
    jam_handle_close(s);
    CHECK_ST(unix_listen_within(l, WAIT, 1000), OK);   /* clamped to BACKLOG_MAX */
    for (unsigned i = 0; i < UNIX_BACKLOG_MAX; i++)
        CHECK_ST(connect_abs(o, "utest-b", &c[i]), OK);
    CHECK_ST(connect_abs(o, "utest-b", &extra), ERR_SHOULD_WAIT);
    /* The listener closes with them waiting: each connector's peer goes. */
    jam_handle_close(l);
    for (unsigned i = 0; i < UNIX_BACKLOG_MAX; i++) {
        CHECK_ST(jam_object_wait_one(c[i], SIG_PEER_CLOSED, now() + WAIT, NULL), OK);
        jam_handle_close(c[i]);
    }
    CHECK_ST(bind_soon(o, "utest-b", &l), OK);   /* and its name is free */
    jam_handle_close(l);
    jam_handle_close(o);
    return true;
}

bool t_unix_limits(void)
{
    static handle_t os[UNIX_OPENERS_MAX + 1], ls[UNIX_LISTENERS_MAX + 1];
    handle_t o = opener();
    if (!o) {
        printf("utest: unix_limits: skipped: no /svc/unix\n");
        return true;
    }
    jam_handle_close(o);
    struct job_info before, after;
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &before), OK);
    /* Listeners: LISTENERS_PER_OPENER on one opener, LISTENERS_MAX in all. */
    unsigned per = UNIX_LISTENERS_PER_OPENER, nopen = UNIX_LISTENERS_MAX / per + 1, nl = 0;
    for (unsigned k = 0; k < nopen; k++)
        CHECK((os[k] = opener()) != HANDLE_INVALID);
    char name[32];
    for (unsigned k = 0; k + 1 < nopen; k++) {
        for (unsigned i = 0; i < per; i++) {
            snprintf(name, sizeof(name), "utest-l%u-%u", k, i);
            CHECK_ST(bind_soon(os[k], name, &ls[nl]), OK);
            nl++;
        }
        snprintf(name, sizeof(name), "utest-l%u-x", k);
        CHECK_ST(bind_abs(os[k], name, &ls[nl]), ERR_NO_RESOURCES);   /* its per-opener limit */
    }
    CHECK_EQ(nl, UNIX_LISTENERS_MAX);
    CHECK_ST(bind_abs(os[nopen - 1], "utest-over", &ls[nl]), ERR_NO_RESOURCES);   /* all of them */
    jam_handle_close(ls[0]);   /* one free again */
    CHECK_ST(bind_soon(os[nopen - 1], "utest-over", &ls[0]), OK);
    for (unsigned i = 0; i < nl; i++)
        jam_handle_close(ls[i]);
    for (unsigned k = 0; k < nopen; k++)
        jam_handle_close(os[k]);
    /* Openers: OPENERS_MAX, then the next refused until one closes. One
     * whose channel closed with a listener bound through it still alive
     * keeps its slot until the listener closes too (CR-REVIEW-POSIX Q2). */
    handle_t kept = opener(), kl = HANDLE_INVALID;
    CHECK(kept != HANDLE_INVALID);
    CHECK_ST(bind_soon(kept, "utest-kept", &kl), OK);
    jam_handle_close(kept);   /* its channel gone, its listener not */
    unsigned n = 0;
    uint64_t until = now() + SOON;   /* the ones above, closed a moment ago */
    while (n < UNIX_OPENERS_MAX - 1 && now() < until) {
        handle_t h = HANDLE_INVALID;
        if (svc_open(SVC_UNIX, &h) == OK)
            os[n++] = h;
        else
            (void)jam_sleep_until(now() + NS_PER_MS);
    }
    CHECK_EQ(n, UNIX_OPENERS_MAX - 1);
    handle_t h = HANDLE_INVALID;
    CHECK_ST(svc_open(SVC_UNIX, &h), ERR_NO_RESOURCES);   /* the kept one's slot counts */
    jam_handle_close(kl);
    until = now() + SOON;
    status_t st;
    while ((st = svc_open(SVC_UNIX, &h)) == ERR_NO_RESOURCES && now() < until)
        (void)jam_sleep_until(now() + NS_PER_MS);
    CHECK_ST(st, OK);   /* its listener gone: its slot is free */
    os[n++] = h;
    CHECK_ST(svc_open(SVC_UNIX, &h), ERR_NO_RESOURCES);
    jam_handle_close(os[0]);
    until = now() + SOON;
    while ((st = svc_open(SVC_UNIX, &h)) == ERR_NO_RESOURCES && now() < until)
        (void)jam_sleep_until(now() + NS_PER_MS);
    CHECK_ST(st, OK);
    os[0] = h;
    for (unsigned k = 0; k < n; k++)
        jam_handle_close(os[k]);
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    return true;
}

/* What ns_send wrote for /svc/unix, read back: its connect byte and its
 * channel (ours now). */
struct sent_svc {
    unsigned n;
    uint8_t  connect;
    handle_t h;
};

static void sent_mount(void *ctx, const uint8_t *paths, uint32_t paths_len,
                       const uint8_t *connect, uint32_t connect_len, const handle_t *hs,
                       uint32_t nh)
{
    struct sent_svc *s = ctx;
    for (uint32_t i = 0; i < nh; i++) {
        bool ours = s->n == 0 && i < connect_len && (i + 1) * NS_NAME_MAX <= paths_len &&
                    !strcmp((const char *)paths + (size_t)i * NS_NAME_MAX, "/svc/" SVC_UNIX);
        if (ours) {
            s->connect = connect[i];
            s->h = hs[i];
            s->n++;
        } else {
            jam_handle_close(hs[i]);
        }
    }
}

/* /svc/unix as ns_send gives it for grants. */
static bool sent_unix(const char *const *grants, struct sent_svc *s)
{
    static const struct nschan_ops ops = { .mount = sent_mount };
    handle_t a, b;
    memset(s, 0, sizeof(*s));
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(ns_send(a, grants), OK);
    CHECK_ST(nschan_serve_one(b, &ops, s), OK);
    jam_handle_close(a);
    jam_handle_close(b);
    CHECK_EQ(s->n, 1u);
    return true;
}

bool t_unix_own(void)
{
    handle_t o = opener();
    if (!o) {
        printf("utest: unix_own: skipped: no /svc/unix\n");
        return true;
    }
    jam_handle_close(o);
    struct job_info before, after;
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &before), OK);
    /* By name: an opener of the program's own, which hands out no more. */
    struct sent_svc s;
    const char *const named[] = { "/svc/" SVC_UNIX, NULL };
    CHECK(sent_unix(named, &s));
    CHECK_EQ(s.connect, 0);
    handle_t l = HANDLE_INVALID, h = HANDLE_INVALID;
    CHECK_ST(bind_abs(s.h, "utest-own", &l), OK);    /* it is an opener */
    CHECK(svc_connect_within(s.h, WAIT, &h) != OK);   /* that makes none: the broker drops it */
    jam_handle_close(l);
    jam_handle_close(s.h);
    /* "*": our channel as we have it, which hands out openers. */
    CHECK(sent_unix(NS_ALL, &s));
    CHECK_EQ(s.connect, 1);
    CHECK_ST(svc_connect_within(s.h, WAIT, &h), OK);
    jam_handle_close(h);
    jam_handle_close(s.h);
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    return true;
}
