/* utest: services under /svc (<os.h> "services") and what a starter gives
 * a child (<os.h> "grants"): names published in our own namespace,
 * listed, opened (a duplicate, or a channel of the opener's own through
 * the svc protocol's connect), kept by svc_get and opened again once the
 * service is replaced; a child given some services and mounts can reach
 * exactly those, and a mount given as a view (read-only, or with its
 * top-level etc kept) refuses what the view refuses. The children are
 * nschild.c's "ns-svc" and "ns-view" modes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <idl/svc.h>
#include <os.h>
#include "utest.h"

#define RAM "/t"

/* One byte written on `to` arrives on `from`. */
static bool passes(handle_t to, handle_t from)
{
    uint8_t b = 0x5a, got = 0;
    uint32_t n = 0;
    CHECK_ST(jam_channel_write(to, &b, 1, NULL, 0), OK);
    struct channel_read_args a = {
        .h = from, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)&got,
        .rd_actual = (uint64_t)(uintptr_t)&n,
    };
    CHECK_ST(jam_channel_read(&a), OK);
    CHECK_EQ(got, 0x5a);
    return true;
}

/* The names under /svc now, as one string "a,b,". */
static void listed(char *out, size_t cap)
{
    struct fs_entry e;
    size_t n = 0;
    out[0] = '\0';
    for (uint32_t i = 0; fs_readdir("/svc", i, &e) == OK && n + strlen(e.name) + 2 < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s,", e.name);
}

bool t_svc_publish_and_open(void)
{
    handle_t cli, srv, cli2, srv2, h, h2;
    CHECK_ST(jam_channel_create(&cli, &srv), OK);
    /* Names: [a-z0-9-], 1 to SVC_NAME_MAX bytes. The handle goes either way. */
    handle_t junk, junk2;
    CHECK_ST(jam_channel_create(&junk, &junk2), OK);
    CHECK_ST(ns_svc_set("T-A", junk, false), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&junk, &junk2), OK);
    CHECK_ST(ns_svc_set("abcdefghijklmnopqrstuvwxy", junk, false), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&junk, &junk2), OK);
    CHECK_ST(ns_svc_set("", junk, false), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(junk2), OK);
    CHECK_ST(ns_svc_set("t-a", cli, false), OK);
    /* Listed, stat-able, not a file. */
    char names[256];
    listed(names, sizeof(names));
    CHECK(strstr(names, "t-a,"));
    bool dir = true, saw = false;
    CHECK_ST(fs_stat("/svc/t-a", NULL, &dir, NULL), OK);
    CHECK(!dir);
    CHECK_ST(fs_stat("/svc", NULL, &dir, NULL), OK);
    CHECK(dir);
    CHECK_ST(fs_stat("/svc/t-zz", NULL, NULL, NULL), ERR_NOT_FOUND);
    struct jfile f;
    CHECK_ST(file_open("/svc/t-a", FS_READ, &f), ERR_WRONG_TYPE);
    struct fs_entry e;
    for (uint32_t i = 0; fs_readdir("/", i, &e) == OK; i++)
        saw |= !strcmp(e.name, "svc");
    CHECK(saw);
    /* Open: a duplicate of the one channel. */
    CHECK_ST(svc_open("t-a", &h), OK);
    CHECK(passes(h, srv));
    CHECK_ST(jam_handle_close(h), OK);
    CHECK_ST(svc_open("t-zz", &h), ERR_NOT_FOUND);
    CHECK_ST(svc_open("No", &h), ERR_INVALID_ARGS);
    /* svc_get keeps one, until its service is gone and another published. */
    h = svc_get("t-a");
    CHECK(h != HANDLE_INVALID);
    CHECK_EQ(svc_get("t-a"), h);
    CHECK_ST(jam_channel_create(&cli2, &srv2), OK);
    CHECK_ST(ns_svc_set("t-a", cli2, false), OK);   /* replaces (closes) cli */
    CHECK_EQ(svc_get("t-a"), h);                    /* the old one still has a peer */
    CHECK_ST(jam_handle_close(srv), OK);            /* the first service ends */
    h2 = svc_get("t-a");
    CHECK(h2 != HANDLE_INVALID && h2 != h);
    CHECK(passes(h2, srv2));
    /* Gone. */
    CHECK_ST(ns_svc_remove("t-a"), OK);
    CHECK_ST(ns_svc_remove("t-a"), ERR_NOT_FOUND);
    CHECK_ST(svc_open("t-a", &h), ERR_NOT_FOUND);
    CHECK_ST(jam_handle_close(srv2), OK);
    return true;
}

/* ---- a service that hands out a channel per opener ------------------------------------- */

#define MADE_MAX 4

static struct {
    handle_t srv;               /* the shared channel's server end */
    handle_t made[MADE_MAX];    /* the server ends of the channels handed out */
    unsigned n;
} fake;

static uint32_t no_protocol(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                            uint32_t *rhn)
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

static status_t fake_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    if (fake.n == MADE_MAX)
        return ERR_NO_RESOURCES;
    return jam_channel_create(&fake.made[fake.n++], out);
}

/* Serve the shared channel until every client end is gone. */
static void fake_main(void *arg)
{
    (void)arg;
    for (;;) {
        status_t st = svc_serve_request(fake.srv, no_protocol, fake_connect, NULL);
        if (st == ERR_PEER_CLOSED)
            return;
        signals_t seen;
        if (st == ERR_SHOULD_WAIT &&
            jam_object_wait_one(fake.srv, SIG_READABLE | SIG_PEER_CLOSED, now() + 30 * NS_PER_S,
                                &seen) != OK)
            return;
    }
}

bool t_svc_connect(void)
{
    static uint8_t stack[16384];
    handle_t cli, shared, th, a, b, c;
    fake.n = 0;
    CHECK_ST(jam_channel_create(&cli, &fake.srv), OK);
    CHECK_ST(jam_handle_duplicate(cli, RIGHT_SAME, &shared), OK);
    CHECK_ST(thread_spawn("fake-svc", fake_main, NULL, stack, sizeof(stack), &th), OK);
    CHECK_ST(ns_svc_set("t-conn", cli, true), OK);
    /* Each open: a channel of its own. */
    CHECK_ST(svc_open("t-conn", &a), OK);
    CHECK_ST(svc_open("t-conn", &b), OK);
    CHECK_EQ(fake.n, 2);
    CHECK(passes(a, fake.made[0]));
    CHECK(passes(b, fake.made[1]));
    /* On the shared channel: connect answered, another protocol refused. */
    CHECK_ST(svc_connect_until(shared, now() + 5 * NS_PER_S, &c), OK);
    CHECK_EQ(fake.n, 3);
    CHECK(passes(c, fake.made[2]));
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(fs_view_until(shared, now() + 5 * NS_PER_S, 0, &c), ERR_NOT_SUPPORTED);
    CHECK_ST(jam_handle_close(shared), OK);
    /* svc_get's is one more, kept. */
    handle_t kept = svc_get("t-conn");
    CHECK(kept != HANDLE_INVALID && kept != a && kept != b);
    CHECK_EQ(fake.n, 4);
    CHECK_EQ(svc_get("t-conn"), kept);
    /* Taken away: the service sees its last client go and ends. */
    CHECK_ST(ns_svc_remove("t-conn"), OK);
    signals_t seen;
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + 10 * NS_PER_S, &seen), OK);
    CHECK_ST(jam_handle_close(th), OK);
    for (unsigned i = 0; i < fake.n; i++)
        CHECK_ST(jam_handle_close(fake.made[i]), OK);
    CHECK_ST(jam_handle_close(fake.srv), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    /* The kept one's peer is gone: asked again it is not found. */
    CHECK_EQ(svc_get("t-conn"), HANDLE_INVALID);
    return true;
}

/* ---- what a child is given --------------------------------------------------------------- */

bool t_svc_child_gets_its_grants(void)
{
    handle_t ca, sa, cb, sb, proc;
    CHECK_ST(jam_channel_create(&ca, &sa), OK);
    CHECK_ST(jam_channel_create(&cb, &sb), OK);
    CHECK_ST(ns_svc_set("t-a", ca, false), OK);
    CHECK_ST(ns_svc_set("t-b", cb, false), OK);
    struct spawn_handle none = { 0, HANDLE_INVALID };
    /* t-a and nothing else: no t-b, no mount. */
    static const char *const only_a[] = { "/svc/t-a", NULL };
    CHECK_ST(ns_child_start("ns-svc", "t-a", only_a, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* Nothing: no /svc at all. */
    static const char *const nothing[] = { NULL };
    CHECK_ST(ns_child_start("ns-svc", "-", nothing, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* A grant may not name a service with a view, or by a pattern. */
    static const char *const bad[] = { "/svc/t-a:r", "/svc/t*", NULL };
    CHECK_ST(ns_child_start("ns-svc", "-", bad, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    CHECK_ST(ns_svc_remove("t-a"), OK);
    CHECK_ST(ns_svc_remove("t-b"), OK);
    CHECK_ST(jam_handle_close(sa), OK);
    CHECK_ST(jam_handle_close(sb), OK);
    return true;
}

/* A child given /t as a view: "r" read-only, "w" writable, both without
 * etc; "rk" read-only with etc/ssh and etc/authorized_keys shown, "ra" with
 * authorized_keys alone (ns-view checks each from the inside). */
bool t_svc_child_gets_views(void)
{
    struct ram r;
    if (!ram_start(RAM, &r))
        return false;
    CHECK_ST(ns_put(RAM "/hello", NS_HELLO), OK);
    CHECK_ST(fs_mkdir(RAM "/etc"), OK);
    CHECK_ST(ns_put(RAM "/etc/allow", NS_HELLO), OK);
    CHECK_ST(ns_put(RAM "/etc/authorized_keys", NS_HELLO), OK);
    CHECK_ST(fs_mkdir(RAM "/etc/ssh"), OK);
    CHECK_ST(ns_put(RAM "/etc/ssh/key", NS_HELLO), OK);
    handle_t proc;
    struct spawn_handle none = { 0, HANDLE_INVALID };
    static const char *const ro[] = { RAM ":r", NULL };
    CHECK_ST(ns_child_start("ns-view", "r", ro, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    static const char *const guarded[] = { RAM ":w", NULL };
    CHECK_ST(ns_child_start("ns-view", "w", guarded, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* Every mount, read-only: the same as /t:r for this child. */
    static const char *const all_ro[] = { "*:r", NULL };
    CHECK_ST(ns_child_start("ns-view", "r", all_ro, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* The SSH server's view: read-only, etc/ssh and authorized_keys shown;
     * its key check's: authorized_keys alone. */
    static const char *const keys[] = { RAM ":rk", NULL };
    CHECK_ST(ns_child_start("ns-view", "rk", keys, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    static const char *const auth[] = { RAM ":ra", NULL };
    CHECK_ST(ns_child_start("ns-view", "ra", auth, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* What the writable child wrote is there; etc is as it was. */
    CHECK(ns_holds(RAM "/new", NS_HELLO));
    CHECK(ns_holds(RAM "/etc/allow", NS_HELLO));
    CHECK_ST(fs_unlink(RAM "/new"), OK);
    CHECK_ST(fs_unlink(RAM "/etc/allow"), OK);
    CHECK_ST(fs_unlink(RAM "/etc/authorized_keys"), OK);
    CHECK_ST(fs_unlink(RAM "/etc/ssh/key"), OK);
    CHECK_ST(fs_unlink(RAM "/etc/ssh"), OK);
    CHECK_ST(fs_unlink(RAM "/etc"), OK);
    return ram_stop(RAM, &r);
}

/* ---- a shared end that one holder never reads ---------------------------------------- */

/* Do we have /svc/<name>? */
static bool have_svc(const char *name)
{
    char got[SVC_NAME_MAX + 1];
    for (unsigned i = 0; ns_svc_at(i, got); i++)
        if (!strcmp(got, name))
            return true;
    return false;
}

/* A service's shared channel is served under the generated unread bound
 * (8 answers or 8 KiB unread: it stops reading the channel), and its
 * client end is every holder's at once. A child (nschild.c's "ns-flood")
 * writes requests with txids of its own on the shared ends of the music
 * player and the mixer (what any program whose list says `svc music` or
 * `svc audio` may do), and exits without reading: the answers wait on
 * ends init keeps for the boot. Each service must still open
 * (svc.connect, its blocking call: answered straight to us, never left on
 * the end). Twice, so a service that came back once is tried again.
 * M12.1 part 1b's finding 1: init reads what lands on its shared ends.
 * Needs the shell's utest (its list's `svc music`, `svc audio`); init's
 * run has neither, and skips. */
bool t_svc_shared_unread(void)
{
    static const char *const names[] = { SVC_MUSIC, SVC_AUDIO };
    char paths[2][NS_NAME_MAX];
    const char *grants[3];
    unsigned n = 0;
    for (unsigned i = 0; i < 2; i++)
        if (have_svc(names[i])) {
            snprintf(paths[n], sizeof(paths[n]), "/svc/%s", names[i]);
            grants[n] = paths[n];
            n++;
        }
    grants[n] = NULL;
    if (!n) {
        printf("utest: %s: neither music nor audio (not the shell's utest?): skipped\n",
               utest_cur);
        return true;
    }
    struct spawn_handle none = { 0, HANDLE_INVALID };
    for (unsigned round = 0; round < 2; round++) {
        handle_t proc = HANDLE_INVALID;
        /* The child's grants carry the shared client ends. A channel end
         * whose queue holds a channel can't be sent in a message (the
         * kernel refuses it, ERR_NOT_SUPPORTED, so that no cycle of
         * channels can be made), and what the last flood made the services
         * answer is exactly that until init has dropped it: the service
         * resumes at each drop and answers the requests it had left, so
         * for a moment (a loaded machine: the soak's) the next spawn
         * finds channels queued. Try again for a while; a queue that
         * stays full is the failure. */
        status_t st = ns_child_start("ns-flood", NULL, grants, none, NULL, &proc);
        unsigned tries = 1;
        for (uint64_t until = now() + 5 * NS_PER_S; st == ERR_NOT_SUPPORTED && now() < until;
             tries++) {
            jam_sleep_until(now() + 20 * NS_PER_MS);
            st = ns_child_start("ns-flood", NULL, grants, none, NULL, &proc);
        }
        if (tries > 1)
            printf("utest: %s: round %u: the shared ends still held answers (%u tries to "
                   "start the flood)\n", utest_cur, round, tries);
        CHECK_ST(st, OK);
        CHECK(ns_child_exits(proc, 0));
        for (unsigned i = 0; i < 2; i++) {
            if (!have_svc(names[i]))
                continue;
            /* The openers the flood made fill the service's table for a
             * moment (ERR_NO_RESOURCES): init closes them, and the service
             * frees each at its next turn. A held channel is ERR_TIMED_OUT. */
            handle_t ch = HANDLE_INVALID;
            status_t st = svc_open(names[i], &ch);
            for (uint64_t until = now() + 3 * NS_PER_S; st == ERR_NO_RESOURCES && now() < until;) {
                jam_sleep_until(now() + 20 * NS_PER_MS);
                st = svc_open(names[i], &ch);
            }
            if (st != OK)
                FAIL("round %u: /svc/%s doesn't open after a holder left its answers unread: %s",
                     round, names[i], status_str(st));
            CHECK_ST(jam_handle_close(ch), OK);
        }
    }
    printf("utest: %s: %u shared end(s) flooded twice, each opens\n", utest_cur, n);
    return true;
}

/* ---- the children ------------------------------------------------------------------------ */

/* "ns-svc <name>|-": the one service we were given is <name> ("-": none),
 * and there is no mount. */
int ns_svc_child(const char *name)
{
    char names[256];
    handle_t h;
    bool none = !strcmp(name, "-");
    listed(names, sizeof(names));
    char want[32] = "";
    if (!none)
        snprintf(want, sizeof(want), "%s,", name);
    if (strcmp(names, want))
        return 80;
    if (!none && svc_open(name, &h) != OK)
        return 81;
    if (!none)
        jam_handle_close(h);
    if (svc_open("t-b", &h) != ERR_NOT_FOUND)
        return 82;
    struct fs_entry e;
    if (fs_readdir("/", 0, &e) != (none ? ERR_NOT_FOUND : OK) || (!none && strcmp(e.name, "svc")))
        return 83;
    if (fs_readdir("/", 1, &e) != ERR_NOT_FOUND)
        return 84;
    return 0;
}

/* Whether the listing of dir names `name`. */
static bool lists(const char *dir, const char *name)
{
    struct fs_entry e;
    for (uint32_t i = 0; fs_readdir(dir, i, &e) == OK; i++)
        if (!strcmp(e.name, name))
            return true;
    return false;
}

/* "ns-view r|w|rk|ra": /t as a program's view, read-only or writable,
 * where etc doesn't exist; or the SSH server's (`:rk`: etc/ssh and
 * etc/authorized_keys) or its key check's (`:ra`: authorized_keys alone),
 * read-only and nothing else of etc. */
int ns_view_child(const char *how)
{
    bool ro = how[0] == 'r', keys = !strcmp(how, "rk"), sshd = keys || !strcmp(how, "ra");
    bool read_only = false;
    struct jfile f;
    if (!ns_holds(RAM "/hello", NS_HELLO))
        return 90;
    if (fs_statfs(RAM "/", NULL, NULL, &read_only, NULL) != OK || read_only != ro)
        return 91;
    status_t want = ro ? ERR_ACCESS_DENIED : OK;
    if (ns_put(RAM "/new", NS_HELLO) != want)
        return 92;
    /* etc: nothing in it changes, however it is named. */
    static const char *const etc[] = { RAM "/etc/allow", RAM "/ETC/allow", RAM "/x/../etc/allow",
                                       RAM "/./Etc/new" };
    for (unsigned i = 0; i < sizeof(etc) / sizeof(etc[0]); i++)
        if (file_open(etc[i], FS_WRITE | FS_CREATE, &f) != ERR_ACCESS_DENIED)
            return 93;
    if (fs_unlink(RAM "/etc/allow") != ERR_ACCESS_DENIED || fs_mkdir(RAM "/etc/d") !=
        ERR_ACCESS_DENIED || fs_rename(RAM "/etc", RAM "/old") != ERR_ACCESS_DENIED)
        return 94;
    if (!ro && fs_rename(RAM "/new", RAM "/etc/new") != ERR_ACCESS_DENIED)
        return 95;
    /* The rest of etc: no view of a program or of the SSH server reads,
     * stats or lists it, however it is named. */
    static const char *const rest[] = { RAM "/etc/allow", RAM "/ETC/allow",
                                        RAM "/x/../etc/allow", RAM "/Etc./allow" };
    for (unsigned i = 0; i < sizeof(rest) / sizeof(rest[0]); i++)
        if (file_open(rest[i], FS_READ, &f) != ERR_ACCESS_DENIED ||
            fs_stat(rest[i], NULL, NULL, NULL) != ERR_ACCESS_DENIED)
            return 96;
    struct fs_entry e;
    if (fs_readdir(RAM "/etc", 0, &e) != ERR_ACCESS_DENIED)
        return 99;
    /* etc itself: a program's view has none (no stat, not in the root's
     * listing); the SSH server's stats it and lists it. */
    bool dir = false;
    status_t est = fs_stat(RAM "/etc", NULL, &dir, NULL);
    if (sshd ? est != OK || !dir || !lists(RAM, "etc")
             : est != ERR_ACCESS_DENIED || fs_stat(RAM "/ETC", NULL, NULL, NULL) !=
                   ERR_ACCESS_DENIED || lists(RAM, "etc") || !lists(RAM, "hello"))
        return 100;
    /* authorized_keys only through the SSH server's two. */
    status_t ast = file_open(RAM "/etc/authorized_keys", FS_READ, &f);
    if (ast == OK)
        file_close(&f);
    if (sshd ? ast != OK : ast != ERR_ACCESS_DENIED)
        return 101;
    /* etc/ssh (the host key) only through `:rk`: read, stat or listed,
     * however it is named (the odd spellings are only for the refusal,
     * which comes before the RAM filesystem walks them: its names have a
     * case). */
    static const char *const key[] = { RAM "/etc/ssh/key", RAM "/etc/ssh",
                                       RAM "/x/../etc/ssh/key", RAM "/ETC/Ssh./key" };
    for (unsigned i = 0; i < sizeof(key) / sizeof(key[0]); i++) {
        status_t st = fs_stat(key[i], NULL, NULL, NULL);
        if (keys ? i < 2 && st != OK : st != ERR_ACCESS_DENIED)
            return 97;
    }
    status_t st = file_open(RAM "/etc/ssh/key", FS_READ, &f);
    if (st == OK)
        file_close(&f);
    status_t lst = fs_readdir(RAM "/etc/ssh", 0, &e);
    if (keys ? (st != OK || lst != OK) : (st != ERR_ACCESS_DENIED || lst != ERR_ACCESS_DENIED))
        return 98;
    return 0;
}
