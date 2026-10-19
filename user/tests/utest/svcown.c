/* utest: the services that count a program by the channel it calls on
 * (netstack's /svc/net and /svc/net-listen, the resolver's /svc/dns, the
 * mixer's /svc/audio and /svc/audioctl; docs/CR-REVIEW-LAYERS.md
 * finding 1): a grant that names one gives the program an opener of its
 * own, which hands out no more, so one program can't take the service's
 * whole table of openers and leave the next program none.
 *
 * t_svc_own_openers starts "utest own-starter" with nothing, which
 * publishes a fake of each service (a table of OWN_TABLE openers, never
 * freed: what a program took stays taken) and starts, each given the five
 * by name:
 *   own-hog   opens each OWN_HOG times, or until refused, and ends;
 *   own-next  then opens each once: it must get them.
 * With an opener of each program's own, the hog's opens all hand back its
 * one opener (each fake made one for it) and the next program gets one of
 * its own (two made). Without, the hog's opens fill each table and the
 * next program is refused. A third child given "*" (as init gives the
 * shells) still opens a new opener of each on the fake's shared channel. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <os.h>
#include "utest.h"

#define OWN_TABLE 4u                    /* openers a fake hands out */
#define OWN_HOG   (2 * OWN_TABLE + 1)   /* opens the hog makes of each: past the table */

static const char *const own_names[] = { SVC_NET, SVC_NET_LISTEN, SVC_DNS, SVC_AUDIO,
                                         SVC_AUDIOCTL };
#define OWN_SVCS (sizeof(own_names) / sizeof(own_names[0]))

static const char *const own_grants[] = { "/svc/" SVC_NET, "/svc/" SVC_NET_LISTEN,
                                          "/svc/" SVC_DNS, "/svc/" SVC_AUDIO,
                                          "/svc/" SVC_AUDIOCTL, NULL };

/* A fake service: its shared channel's server end, and the server ends of
 * the openers it made (n of them; the fakes' thread writes n). */
static struct own_fake {
    handle_t srv;
    handle_t made[OWN_TABLE];
    unsigned n;
} fakes[OWN_SVCS];
static bool fakes_stop;   /* atomic: the starter sets it, the thread ends */

static uint32_t own_no_protocol(void *ctx, const void *req, uint32_t n, void *rep,
                                handle_t *rhs, uint32_t *rhn)
{
    (void)ctx, (void)rhs;
    *rhn = 0;
    if (n < 4)
        return 0;
    struct idl_rep_hdr *r = rep;
    r->txid = ((const struct idl_req_hdr *)req)->txid;
    r->status = ERR_NOT_SUPPORTED;
    return sizeof(*r);
}

static status_t own_connect(void *ctx, handle_t *out)
{
    struct own_fake *f = ctx;
    unsigned n = __atomic_load_n(&f->n, __ATOMIC_ACQUIRE);
    if (n == OWN_TABLE)
        return ERR_NO_RESOURCES;
    status_t st = jam_channel_create(&f->made[n], out);
    if (st == OK)
        __atomic_store_n(&f->n, n + 1, __ATOMIC_RELEASE);
    return st;
}

/* Serve every fake's shared channel until told to stop. */
static void fakes_main(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&fakes_stop, __ATOMIC_ACQUIRE)) {
        bool served = false;
        for (unsigned i = 0; i < OWN_SVCS; i++)
            served |= svc_serve_request(fakes[i].srv, own_no_protocol, own_connect,
                                        &fakes[i]) == OK;
        if (!served)
            (void)jam_sleep_until(now() + NS_PER_MS);
    }
}

/* What each fake has made, as "net 1, net-listen 1, ...", into out. */
static void made_text(char *out, size_t cap)
{
    size_t at = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < OWN_SVCS && at < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, "%s%s %u", i ? ", " : "", own_names[i],
                               __atomic_load_n(&fakes[i].n, __ATOMIC_ACQUIRE));
}

/* Every fake has made `want` openers. */
static bool made_each(unsigned want)
{
    for (unsigned i = 0; i < OWN_SVCS; i++)
        if (__atomic_load_n(&fakes[i].n, __ATOMIC_ACQUIRE) != want)
            return false;
    return true;
}

/* Start "utest <mode>" with grants and wait for its exit code (-1: it
 * didn't start or end). */
static int64_t own_run(const char *mode, const char *const *grants)
{
    struct spawn_handle none = { 0, HANDLE_INVALID };
    struct process_info info;
    handle_t proc;
    if (ns_child_start(mode, NULL, grants, none, NULL, &proc) != OK)
        return -1;
    status_t st = spawn_wait(proc, 20 * NS_PER_S, &info);
    jam_handle_close(proc);
    return st == OK && !info.killed ? info.exit_code : -1;
}

/* "own-starter": the fakes, the hog, the next program, one given "*". */
static int own_starter(void)
{
    static uint8_t stack[16384] __attribute__((aligned(64)));
    for (unsigned i = 0; i < OWN_SVCS; i++) {
        handle_t cli;
        if (jam_channel_create(&cli, &fakes[i].srv) != OK ||
            ns_svc_set(own_names[i], cli, true) != OK)
            return 60;
    }
    handle_t th;
    if (thread_spawn("own-fakes", fakes_main, NULL, stack, sizeof(stack), &th) != OK)
        return 61;
    char made[128];
    int code = 0;
    int64_t hog = own_run("own-hog", own_grants);
    made_text(made, sizeof(made));
    printf("utest: own-starter: the hog ended (%ld); openers made: %s\n", (long)hog, made);
    int64_t next = own_run("own-next", own_grants);
    made_text(made, sizeof(made));
    if (hog != 0) {
        code = 62;
    } else if (next != 0) {
        printf("utest: own-starter: the next program got none (%ld): the hog took the "
               "tables; openers made: %s\n", (long)next, made);
        code = 64;
    } else if (!made_each(2)) {
        printf("utest: own-starter: want one opener a program; made: %s\n", made);
        code = 65;
    }
    if (!code && own_run("own-next", NS_ALL) != 0)
        code = 66;
    else if (!code && !made_each(3))
        code = 67;   /* "*" hands out the shared channel: a new opener */
    __atomic_store_n(&fakes_stop, true, __ATOMIC_RELEASE);
    signals_t seen;
    (void)jam_object_wait_one(th, SIG_TERMINATED, now() + 10 * NS_PER_S, &seen);
    jam_handle_close(th);
    return code;
}

/* "own-hog": open each service OWN_HOG times, or until it is refused. */
static int own_hog(void)
{
    for (unsigned i = 0; i < OWN_SVCS; i++) {
        handle_t h[OWN_HOG];
        unsigned n = 0;
        status_t st = OK;
        while (n < OWN_HOG && (st = svc_open(own_names[i], &h[n])) == OK)
            n++;
        printf("utest: own-hog: %s opened %u times%s%s\n", own_names[i], n,
               st == OK ? "" : ", then ", st == OK ? "" : status_str(st));
        for (unsigned k = 0; k < n; k++)
            jam_handle_close(h[k]);
        if (!n)
            return 70 + (int)i;
    }
    return 0;
}

/* "own-next": each service opens. */
static int own_next(void)
{
    for (unsigned i = 0; i < OWN_SVCS; i++) {
        handle_t h;
        status_t st = svc_open(own_names[i], &h);
        if (st != OK) {
            printf("utest: own-next: %s: %s\n", own_names[i], status_str(st));
            return 80 + (int)i;
        }
        jam_handle_close(h);
    }
    return 0;
}

int svc_own_child(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "own-starter"))
        return own_starter();
    if (!strcmp(argv[1], "own-hog"))
        return own_hog();
    if (!strcmp(argv[1], "own-next"))
        return own_next();
    return 127;
}

bool t_svc_own_openers(void)
{
    static const char *const nothing[] = { NULL };
    struct spawn_handle none = { 0, HANDLE_INVALID };
    handle_t proc;
    CHECK_ST(ns_child_start("own-starter", NULL, nothing, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    return true;
}
