/* utest: a service's state VMO and warm spares (<svcstate.h>).
 *
 * This process plays the supervisor (it makes the VMO and hands out a
 * service's handle on it) and the service (it maps it at the fixed
 * address); a "death" is unmapping it, a successor is mapping it again.
 * The state: new, adopted, its rights; requests through the two slots and
 * what a successor finds after a death at each step; every check that
 * refuses a corrupted or foreign state. Spares are real children
 * ("utest svcstate-spare"): promoted, dismissed, given a bad promotion,
 * killed while they wait.
 *
 * The child modes are here too, with the reader of the kernel's
 * chanread_kill_loses_nothing (kernel/test/test_chanread.c, which has the
 * reason; the log VMO's layout is the CR_* below, the same as there). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <svcstate.h>
#include "utest.h"

#define KIND   0x74736574u   /* "test" */
#define SPARE_WAIT (10 * NS_PER_S)

static const struct svcstate_layout L = {
    .kind = KIND, .layout = 1, .binding = "disk 1, partition 2",
    .req_cap = 512, .rep_cap = 256, .user_size = 3 * PAGE_SIZE,
};

static uint64_t handles_used(void)
{
    struct job_info ji;
    return info_of(own_job(), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES] : 0;
}

/* The service's handle on state, as a supervisor sends it: through a
 * channel with SVCSTATE_SERVICE_RIGHTS. */
static status_t service_handle(handle_t state, handle_t *out)
{
    handle_t give, a, b;
    status_t st = svcstate_give(state, &give);
    if (st != OK)
        return st;
    st = jam_channel_create(&a, &b);
    if (st != OK) {
        jam_handle_close(give);
        return st;
    }
    rights_t r = SVCSTATE_SERVICE_RIGHTS;
    st = jam_channel_write_rights(a, "", 0, &give, &r, 1);
    if (st != OK)
        jam_handle_close(give);
    uint32_t nb = 0, nh = 0;
    struct channel_read_args rd = {
        .h = b, .handles = (uint64_t)(uintptr_t)out, .handles_cap = 1,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    if (st == OK)
        st = jam_channel_read(&rd);
    jam_handle_close(a);
    jam_handle_close(b);
    return st == OK && nh != 1 ? ERR_BAD_STATE : st;
}

/* A new state VMO for L: the supervisor's handle and the service's. */
static bool new_state(handle_t *state, handle_t *svc)
{
    /* A test that failed before us may have left its state mapped there
     * (ERR_NOT_FOUND: nothing was). */
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), SVCSTATE_ADDR, SVCSTATE_MAX_SIZE);
    CHECK_ST(svcstate_create(svcstate_size(&L), state), OK);
    CHECK_ST(service_handle(*state, svc), OK);
    return true;
}

/* Map svc as a successor would and say what it found. */
static bool reopen(struct svcstate *s, handle_t svc, enum svcstate_start want)
{
    svcstate_close(s);
    enum svcstate_start how;
    CHECK_ST(svcstate_open(svc, &L, s, &how), OK);
    CHECK_EQ(how, want);
    return true;
}

/* A new state, its fixed address and its header; the rights the service
 * gets (no duplicate, transfer or resize); adopted by each successor, the
 * service's own area kept; layouts and VMOs that can't be used. */
bool t_svcstate_fresh_and_adopted(void)
{
    uint64_t before = handles_used();
    handle_t state, svc, x, y;
    CHECK(new_state(&state, &svc));
    CHECK_ST(jam_handle_duplicate(svc, RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_set_size(svc, 2 * svcstate_size(&L)), ERR_ACCESS_DENIED);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_channel_write(x, "", 0, &svc, 1), ERR_ACCESS_DENIED);
    jam_handle_close(x);
    jam_handle_close(y);

    struct svcstate s = { 0 }, t = { 0 };
    enum svcstate_start how;
    CHECK_ST(svcstate_open(svc, &L, &s, &how), OK);
    CHECK_EQ(how, SVCSTATE_FRESH);
    CHECK((uint64_t)(uintptr_t)s.h == SVCSTATE_ADDR);
    CHECK(s.h->magic == SVCSTATE_MAGIC && s.h->kind == KIND && s.h->adopted == 0);
    CHECK(s.h->size == svcstate_size(&L) && s.h->size == 8 * PAGE_SIZE);   /* 1 + 2 * 2 + 3 */
    CHECK((uint64_t)(uintptr_t)svcstate_user(&s) == SVCSTATE_ADDR + 5 * PAGE_SIZE);
    memcpy(svcstate_user(&s), "mine", 5);
    CHECK_ST(svcstate_open(svc, &L, &t, &how), ERR_ALREADY_BOUND);   /* the address is taken */

    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK(s.h->adopted == 1 && !strcmp(svcstate_user(&s), "mine"));
    unsigned slot = 9;
    CHECK_EQ(svcstate_pending(&s, &slot), SVCSTATE_IDLE);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK(s.h->adopted == 2);
    svcstate_close(&s);

    struct svcstate_layout bad = L;
    bad.req_cap = 2;
    CHECK_EQ(svcstate_size(&bad), 0);
    CHECK_ST(svcstate_open(svc, &bad, &s, &how), ERR_OUT_OF_RANGE);
    bad = L;
    bad.user_size = SVCSTATE_MAX_SIZE;
    CHECK_EQ(svcstate_size(&bad), 0);
    bad = L;
    bad.user_size = 4 * PAGE_SIZE;   /* more than this VMO holds */
    CHECK_ST(svcstate_open(svc, &bad, &s, &how), ERR_OUT_OF_RANGE);
    CHECK_ST(svcstate_create(SVCSTATE_MAX_SIZE + 1, &x), ERR_OUT_OF_RANGE);
    jam_handle_close(svc);
    jam_handle_close(state);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* Write a request {txid, tag, padding to n bytes} on cli. */
static bool ask(handle_t cli, uint32_t txid, uint32_t tag, uint32_t n)
{
    uint32_t m[160] = { txid, tag };
    CHECK(n <= sizeof(m));
    CHECK_ST(jam_channel_write(cli, m, n, NULL, 0), OK);
    return true;
}

/* The slot holds request {txid, tag} of n bytes. */
static bool holds(const struct svcstate *s, unsigned slot, uint32_t txid, uint32_t tag,
                  uint32_t n)
{
    uint32_t len = 0;
    const uint32_t *m = svcstate_request(s, slot, &len);
    CHECK(len == n && m[0] == txid && m[1] == tag);
    return true;
}

/* Answer slot with {txid, value}: commit only. */
static bool commit(struct svcstate *s, unsigned slot, uint32_t txid, uint32_t value)
{
    uint32_t *r = svcstate_reply_area(s, slot);
    r[0] = txid;
    r[1] = value;
    CHECK_ST(svcstate_commit(s, slot, 8), OK);
    return true;
}

/* cli has reply {txid, value}. */
static bool answered(handle_t cli, uint32_t txid, uint32_t value)
{
    uint32_t r[2] = { 0 }, nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = cli, .bytes_cap = sizeof(r), .bytes = (uint64_t)(uintptr_t)r,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&a), OK);
    CHECK(nb == 8 && r[0] == txid && r[1] == value);
    return true;
}

/* A request through the slots, and what a successor finds after a death
 * at each step: taken (re-run it from the slot), committed (send, then
 * reply), sent (reply), set up with nothing read (idle); the two slots
 * alternating across successors; a request too big or too short. */
bool t_svcstate_slots(void)
{
    uint64_t before = handles_used();
    handle_t state, svc, srv, cli;
    CHECK(new_state(&state, &svc));
    CHECK_ST(jam_channel_create(&srv, &cli), OK);
    struct svcstate s = { 0 };
    enum svcstate_start how;
    CHECK_ST(svcstate_open(svc, &L, &s, &how), OK);
    unsigned slot = 9, found = 9;

    CHECK(ask(cli, 7, 0xa, 40));
    CHECK_ST(svcstate_take(&s, 3, srv, &slot), OK);
    CHECK(slot == 1 && s.h->slot[1].seq == 1 && s.h->slot[1].channel == 3);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_RERUN);
    CHECK(found == 1 && holds(&s, 1, 7, 0xa, 40));
    CHECK(commit(&s, 1, 7, 0xaa));
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_RESEND);
    svcstate_sent(&s, 1);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_REPLY);
    CHECK_ST(svcstate_reply(&s, 1, srv, NULL, 0), OK);
    CHECK(answered(cli, 7, 0xaa));
    /* Killed before the next read: the reply is marked out, nothing is owed. */
    CHECK(s.h->slot[1].replied == 1);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_IDLE);

    CHECK_ST(svcstate_take(&s, 3, srv, &slot), ERR_SHOULD_WAIT);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_IDLE);   /* slot 0 set up, nothing read */
    CHECK(ask(cli, 8, 0xb, 12));
    CHECK_ST(svcstate_take(&s, 4, srv, &slot), OK);
    CHECK(slot == 0 && s.h->slot[0].seq == 2);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_RERUN);
    CHECK(found == 0 && holds(&s, 0, 8, 0xb, 12));
    uint32_t *r = svcstate_reply_area(&s, 0);
    r[0] = 8;
    r[1] = 0xbb;
    s.h->slot[0].reply_len = 8;
    CHECK_ST(svcstate_reply(&s, 0, srv, NULL, 0), OK);   /* commits and marks sent itself */
    CHECK(answered(cli, 8, 0xbb));
    CHECK(s.h->commit == 2 && s.h->slot[0].phase == SVCSTATE_SENT);

    CHECK(ask(cli, 9, 0xc, 600));   /* over req_cap: stays queued, the slot empty */
    CHECK_ST(svcstate_take(&s, 3, srv, &slot), ERR_BUFFER_TOO_SMALL);
    CHECK(s.h->slot[1].len == 0 && s.h->slot[1].seq == 3);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_IDLE);
    uint8_t big[600];
    uint32_t nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = srv, .bytes_cap = sizeof(big), .bytes = (uint64_t)(uintptr_t)big,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&a), OK);   /* the service throws it away */
    CHECK_ST(jam_channel_write(cli, "ab", 2, NULL, 0), OK);   /* no txid */
    CHECK_ST(svcstate_take(&s, 3, srv, &slot), ERR_INVALID_ARGS);
    CHECK(ask(cli, 10, 0xd, 8));
    CHECK_ST(svcstate_take(&s, 3, srv, &slot), OK);
    CHECK(slot == 1 && s.h->slot[1].seq == 3);   /* the number nothing used */
    CHECK_ST(svcstate_commit(&s, slot, L.rep_cap + 1), ERR_OUT_OF_RANGE);

    svcstate_close(&s);
    jam_handle_close(srv);
    jam_handle_close(cli);
    jam_handle_close(svc);
    jam_handle_close(state);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* A reply that waits for the service's next system call (svcstate_answer,
 * <idl/common.h> struct idl_reply), marked out in its slot by the kernel
 * in the call that sends it. A death before that call, the next slot set
 * up: the reply is owed (svcstate_pending says REPLY for the older slot);
 * after it: nothing is. The next request read in the call that sends the
 * reply. A reply to a client that has gone is never marked out, and its
 * handle is closed; one of 0 bytes waits for nothing. */
bool t_svcstate_answer_mark(void)
{
    uint64_t before = handles_used();
    handle_t state, svc, srv, cli, ev;
    CHECK(new_state(&state, &svc));
    CHECK_ST(jam_channel_create(&srv, &cli), OK);
    struct svcstate s = { 0 };
    enum svcstate_start how;
    CHECK_ST(svcstate_open(svc, &L, &s, &how), OK);
    struct idl_reply rep = { .ch = HANDLE_INVALID };
    struct idl_slot is;
    unsigned slot = 9, next = 9, found = 9;

    CHECK(ask(cli, 7, 0xa, 40));
    svcstate_prepare(&s, 3, &slot, &is);
    CHECK_ST(idl_take(srv, &is), OK);
    CHECK(svcstate_taken(&s, slot) && slot == 1 && holds(&s, 1, 7, 0xa, 40));
    CHECK(commit(&s, slot, 7, 0xaa));
    svcstate_answer(&s, slot, srv, NULL, 0, &rep);
    CHECK(rep.ch == srv && rep.rn == 8 && rep.mark == &s.h->slot[1].replied);
    CHECK(s.h->slot[1].phase == SVCSTATE_SENT && s.h->slot[1].replied == 0);
    svcstate_prepare(&s, 3, &next, &is);   /* then killed */
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_REPLY);
    CHECK_EQ(found, 1);
    svcstate_prepare(&s, 3, &next, &is);
    CHECK_ST(idl_take_after(srv, &is, &rep), ERR_SHOULD_WAIT);   /* sends it, finds nothing */
    CHECK(!svcstate_taken(&s, next) && rep.ch == HANDLE_INVALID && s.h->slot[1].replied == 1);
    CHECK(answered(cli, 7, 0xaa));
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_IDLE);

    CHECK(ask(cli, 8, 0xb, 12));
    svcstate_prepare(&s, 3, &slot, &is);
    CHECK_ST(idl_take(srv, &is), OK);
    CHECK(svcstate_taken(&s, slot) && slot == 0);
    CHECK(commit(&s, slot, 8, 0xbb));
    svcstate_answer(&s, slot, srv, NULL, 0, &rep);
    CHECK(ask(cli, 9, 0xc, 16));
    svcstate_prepare(&s, 3, &next, &is);
    CHECK_ST(idl_take_after(srv, &is, &rep), OK);   /* 8's reply out, 9 in */
    CHECK(svcstate_taken(&s, next) && next == 1 && s.h->slot[0].replied == 1);
    CHECK(holds(&s, 1, 9, 0xc, 16) && answered(cli, 8, 0xbb));
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_RERUN);

    CHECK(commit(&s, 1, 9, 0xcc));
    CHECK_ST(jam_event_create(&ev), OK);
    svcstate_answer(&s, 1, srv, &ev, 1, &rep);
    CHECK_ST(jam_handle_close(cli), OK);
    svcstate_prepare(&s, 3, &next, &is);
    CHECK_ST(idl_take_after(srv, &is, &rep), ERR_PEER_CLOSED);
    CHECK(s.h->slot[1].replied == 0 && rep.ch == HANDLE_INVALID);
    CHECK(reopen(&s, svc, SVCSTATE_ADOPTED));
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_REPLY);   /* to nobody: harmless */
    CHECK_ST(svcstate_commit(&s, 1, 0), OK);   /* answered later, say */
    svcstate_answer(&s, 1, srv, NULL, 0, &rep);
    CHECK(rep.ch == HANDLE_INVALID);
    CHECK_EQ(svcstate_pending(&s, &found), SVCSTATE_IDLE);

    svcstate_close(&s);
    jam_handle_close(srv);
    jam_handle_close(svc);
    jam_handle_close(state);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* A new state with a request answered in slot 1 and one taken in slot 0,
 * so every slot field has something to corrupt; then unmapped. */
static bool used_state(handle_t state, handle_t svc)
{
    static const uint8_t zeros[sizeof(struct svcstate_header)];
    CHECK_ST(jam_vmo_write(state, 0, zeros, sizeof(zeros)), OK);   /* empty: no other case's */
    handle_t srv, cli;
    CHECK_ST(jam_channel_create(&srv, &cli), OK);
    struct svcstate s = { 0 };
    enum svcstate_start how;
    unsigned slot;
    CHECK_ST(svcstate_open(svc, &L, &s, &how), OK);
    CHECK_EQ(how, SVCSTATE_FRESH);
    CHECK(ask(cli, 1, 1, 8) && ask(cli, 2, 2, 8));
    CHECK_ST(svcstate_take(&s, 0, srv, &slot), OK);
    CHECK(commit(&s, slot, 1, 1));
    CHECK_ST(svcstate_reply(&s, slot, srv, NULL, 0), OK);
    CHECK_ST(svcstate_take(&s, 0, srv, &slot), OK);
    svcstate_close(&s);
    jam_handle_close(srv);
    jam_handle_close(cli);
    return true;
}

/* A successor expecting w refuses the state for `why` and sets it up
 * empty for w (the next successor adopts that). */
static bool refused_now(handle_t svc, const struct svcstate_layout *w, const char *why)
{
    struct svcstate s = { 0 };
    enum svcstate_start how;
    CHECK_ST(svcstate_open(svc, w, &s, &how), OK);
    if (how != SVCSTATE_REFUSED || !s.why || strcmp(s.why, why))
        FAIL("refused for \"%s\", want \"%s\"", s.why ? s.why : "nothing", why);
    CHECK(s.h->adopted == 0 && s.h->commit == 0 && !s.h->slot[0].seq && !s.h->slot[1].seq);
    svcstate_close(&s);
    CHECK_ST(svcstate_open(svc, w, &s, &how), OK);
    CHECK_EQ(how, SVCSTATE_ADOPTED);
    svcstate_close(&s);
    return true;
}

/* One corruption: len bytes at off of a used state, written through the
 * supervisor's handle (bytes NULL: none), refused by a successor
 * expecting w. */
static bool refused(handle_t state, handle_t svc, uint64_t off, const void *bytes, size_t len,
                    const struct svcstate_layout *w, const char *why)
{
    CHECK(used_state(state, svc));
    if (bytes)
        CHECK_ST(jam_vmo_write(state, off, bytes, len), OK);
    return refused_now(svc, w, why);
}

#define AT(f) offsetof(struct svcstate_header, f)
#define SLOT(i, f) (AT(slot) + (i) * sizeof(struct svcstate_slot) + \
                    offsetof(struct svcstate_slot, f))

/* Every check a successor makes before it uses a state. */
bool t_svcstate_refused(void)
{
    uint64_t before = handles_used();
    handle_t st, sv;
    CHECK(new_state(&st, &sv));
    uint64_t junk = 0x0123456789abcdefull, big = UINT64_MAX;
    uint32_t two = 2, seven = 7, four = 4, toolong = L.rep_cap + 1;
    struct svcstate_layout other = L;
    other.kind = KIND + 1;
    CHECK(refused(st, sv, 0, NULL, 0, &other, "another service's"));
    other = L;
    other.layout = 2;
    CHECK(refused(st, sv, 0, NULL, 0, &other, "another layout version"));
    other = L;
    other.binding[0] = 'D';   /* a state from another disk */
    CHECK(refused(st, sv, 0, NULL, 0, &other, "bound to something else"));
    other = L;
    other.req_cap = 1024;     /* the same pages, another layout */
    CHECK(refused(st, sv, 0, NULL, 0, &other, "another layout"));
    CHECK(refused(st, sv, AT(magic), &junk, 8, &L, "not a state"));
    CHECK(refused(st, sv, AT(version), &seven, 4, &L, "another header version"));
    CHECK(refused(st, sv, AT(header_size), &two, 4, &L, "another header version"));
    CHECK(refused(st, sv, AT(size), &junk, 8, &L, "another layout"));
    CHECK(refused(st, sv, AT(adopted), &big, 8, &L, "adopted too often"));
    CHECK(refused(st, sv, SLOT(0, seq), &seven, 4, &L, "a request in the wrong slot"));
    CHECK(refused(st, sv, SLOT(0, seq), &four, 4, &L, "slots out of step"));
    CHECK(refused(st, sv, SLOT(1, phase), &seven, 4, &L, "a slot in no known phase"));
    CHECK(refused(st, sv, SLOT(1, reply_len), &toolong, 4, &L, "a reply longer than its area"));
    CHECK(refused(st, sv, AT(commit), &seven, 8, &L, "a commit word out of step"));
    uint64_t two64 = 2, one64 = 1;   /* slot 1's reply is out (1); slot 0 is still running */
    CHECK(refused(st, sv, SLOT(1, replied), &two64, 8, &L, "a reply out before it was ready"));
    CHECK(refused(st, sv, SLOT(0, replied), &one64, 8, &L, "a reply out before it was ready"));
    /* Slot 1 (seq 1) was answered before slot 0 was set up: the commit
     * word can't be 0, whether slot 1 says it was sent or not. */
    uint64_t zero = 0;
    uint32_t run = SVCSTATE_RUN;
    CHECK(refused(st, sv, AT(commit), &zero, 8, &L, "a request sent before its commit"));
    CHECK(used_state(st, sv));
    CHECK_ST(jam_vmo_write(st, AT(commit), &zero, 8), OK);
    CHECK_ST(jam_vmo_write(st, SLOT(1, phase), &run, 4), OK);
    CHECK_ST(jam_vmo_write(st, SLOT(1, replied), &zero, 8), OK);
    CHECK(refused_now(sv, &L, "a commit word out of step"));
    uint32_t sent = SVCSTATE_SENT;   /* slot 0 (seq 2) taken, not committed */
    CHECK(refused(st, sv, SLOT(0, phase), &sent, 4, &L, "a request sent before its commit"));
    jam_handle_close(sv);
    jam_handle_close(st);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* ---- warm spares ----------------------------------------------------------------- */

/* Start "utest svcstate-spare" with SR_STANDBY in a new job: our end of
 * the standby channel in *sup. */
static bool start_spare(handle_t *job, handle_t *sup, handle_t *proc)
{
    handle_t end;
    CHECK_ST(new_job(job), OK);
    CHECK_ST(jam_channel_create(sup, &end), OK);
    const char *argv[] = { "utest", "svcstate-spare" };
    struct spawn_handle x = { SR_STANDBY, end };
    struct spawn_args a = {
        .path = "bin/utest", .name = "utest-spare", .argc = 2, .argv = argv, .job = *job,
        .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&a, proc), OK);
    return true;
}

/* The spare ends with code; its job empty afterwards. */
static bool spare_ends(handle_t job, handle_t proc, int64_t code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(proc, SPARE_WAIT, &info), OK);
    CHECK_EQ(info.exit_code, code);
    jam_handle_close(proc);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    jam_handle_close(job);
    return true;
}

/* What "utest svcstate-promoted" reports. */
struct promoted_report {
    uint32_t txid;      /* 0 */
    uint32_t checks;    /* PR_* that held */
    uint64_t kill_ns;   /* standby_kill_ns() */
};
#define PR_NO_STANDBY 1u    /* SR_STANDBY is gone from the table */
#define PR_EVENT      2u    /* SR_USER + 5 is the event, with its rights */
#define PR_ENV        4u    /* the promotion's environment */
#define PR_ARGV       8u    /* the promotion's argv */
#define PR_WHEN       16u   /* standby_promoted_ns() is set */
#define PR_OWN        32u   /* the spare's own startup handles are still there */
#define PR_ALL        63u

/* Promoted: the handles, argv, environment and kill time of the
 * promotion; dismissed, killed, or given a bad promotion: ended without
 * running main; a promotion that can't fit refused before it is sent. */
bool t_svcstate_standby(void)
{
    uint64_t before = handles_used();
    handle_t job, sup, proc, ev, mine, theirs;
    CHECK(start_spare(&job, &sup, &proc));
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 30 * NS_PER_MS, &info), ERR_TIMED_OUT);   /* waiting */
    CHECK_ST(jam_process_get_info(proc, &info), OK);
    CHECK(info.state != PROCESS_DEAD && info.threads == 1);
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    const char *argv[] = { "utest", "svcstate-promoted", "x" };
    const char *envp[] = { "SPARE=yes", NULL };
    struct spawn_handle hs[] = { { SR_USER + 5, ev }, { SR_USER + 6, theirs } };
    rights_t rights[] = { RIGHTS_BASIC | RIGHT_SIGNAL, RIGHT_SAME };
    struct standby_args pa = { .hs = hs, .rights = rights, .n = 2, .argc = 3, .argv = argv,
                               .envp = envp, .kill_ns = 1234567 };
    CHECK_ST(standby_promote(sup, &pa), OK);
    struct promoted_report rep = { 0 };
    uint32_t nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = mine, .bytes_cap = sizeof(rep), .bytes = (uint64_t)(uintptr_t)&rep,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    signals_t seen;
    CHECK_ST(jam_object_wait_one(mine, SIG_READABLE, now() + SPARE_WAIT, &seen), OK);
    CHECK_ST(jam_channel_read(&a), OK);   /* read first: it is charged to the spare's job */
    CHECK(spare_ends(job, proc, 0));
    if (rep.checks != PR_ALL)
        FAIL("the promoted spare's checks: %x of %x", rep.checks, PR_ALL);
    CHECK(rep.kill_ns == 1234567);
    jam_handle_close(mine);
    jam_handle_close(sup);

    CHECK(start_spare(&job, &sup, &proc));   /* dismissed */
    jam_handle_close(sup);
    CHECK(spare_ends(job, proc, 0));

    CHECK(start_spare(&job, &sup, &proc));   /* killed while it waits */
    CHECK_ST(jam_process_kill(proc), OK);
    CHECK(spare_ends(job, proc, PROCESS_KILLED_CODE));
    jam_handle_close(sup);

    struct standby_msg bad = { .magic = STANDBY_MAGIC + 1, .version = STANDBY_VERSION };
    CHECK(start_spare(&job, &sup, &proc));
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_channel_write(sup, &bad, sizeof(bad), &ev, 1), OK);   /* bad magic */
    CHECK(spare_ends(job, proc, STANDBY_BAD_PROMOTION));
    jam_handle_close(sup);

    CHECK(start_spare(&job, &sup, &proc));   /* a second standby channel in it */
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    struct spawn_handle again = { SR_STANDBY, theirs };
    pa = (struct standby_args){ .hs = &again, .n = 1, .argc = 2, .argv = argv };
    CHECK_ST(standby_promote(sup, &pa), OK);
    CHECK(spare_ends(job, proc, STANDBY_BAD_PROMOTION));
    jam_handle_close(mine);
    jam_handle_close(sup);

    struct spawn_handle many[STANDBY_MAX_HANDLES + 1];
    for (unsigned i = 0; i < STANDBY_MAX_HANDLES + 1; i++) {
        many[i].role = SR_USER;
        CHECK_ST(jam_event_create(&many[i].h), OK);
    }
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    pa = (struct standby_args){ .hs = many, .n = STANDBY_MAX_HANDLES + 1, .argc = 2,
                                .argv = argv };
    CHECK_ST(standby_promote(mine, &pa), ERR_OUT_OF_RANGE);   /* all closed */
    jam_handle_close(mine);
    jam_handle_close(theirs);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* ---- child modes ----------------------------------------------------------------- */

/* "utest svcstate-promoted x" after its promotion (above). */
static int promoted(int argc, char **argv)
{
    struct promoted_report rep = { .kill_ns = standby_kill_ns() };
    if (startup_handle(SR_STANDBY) == HANDLE_INVALID)
        rep.checks |= PR_NO_STANDBY;
    if (jam_event_signal(startup_handle(SR_USER + 5), 0, SIG_SIGNALED) == OK)
        rep.checks |= PR_EVENT;
    for (char **e = environ; *e; e++)
        if (!strcmp(*e, "SPARE=yes"))
            rep.checks |= PR_ENV;
    if (argc == 3 && !strcmp(argv[2], "x"))
        rep.checks |= PR_ARGV;
    if (standby_promoted_ns())
        rep.checks |= PR_WHEN;
    if (startup_handle(SR_SELF_PROCESS) != HANDLE_INVALID &&
        startup_handle(SR_JOB) != HANDLE_INVALID)
        rep.checks |= PR_OWN;
    return jam_channel_write(startup_handle(SR_USER + 6), &rep, sizeof(rep), NULL, 0) == OK
           ? 0 : 1;
}

/* The log VMO's layout: kernel/test/test_chanread.c's CR_*. */
#define CR_ENTRIES 48
#define CR_STRIDE  (20u << 10)
#define CR_DATA    64
#define CR_MSG_MAX (16u << 10)
#define CR_SIZE    (PAGE_SIZE + CR_ENTRIES * CR_STRIDE)

/* "utest svcstate-chanread": read SR_USER's messages, each straight into
 * the next entry of SR_USER + 1 (its length written by the read), and
 * count each in the header after the read returned. */
static int chanread(void)
{
    handle_t ch = startup_handle(SR_USER), log = startup_handle(SR_USER + 1);
    uint64_t addr = 0;
    if (jam_vmar_map(startup_handle(SR_SELF_VMAR), log, 0, CR_SIZE, VMAR_READ | VMAR_WRITE,
                     &addr) != OK)
        return 1;
    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)addr;
    for (uint64_t off = 0; off < CR_SIZE; off += PAGE_SIZE)
        base[off] = base[off];   /* mapped before any read: a copy never faults */
    uint32_t *hdr = (uint32_t *)(uintptr_t)addr;
    __atomic_store_n(&hdr[1], 1, __ATOMIC_RELEASE);   /* ready */
    for (uint32_t k = 0; k < CR_ENTRIES;) {
        uint8_t *e = (uint8_t *)(uintptr_t)(addr + PAGE_SIZE + (uint64_t)k * CR_STRIDE);
        struct channel_read_args a = {
            .h = ch, .bytes_cap = CR_MSG_MAX, .bytes = (uint64_t)(uintptr_t)(e + CR_DATA),
            .actual_bytes = (uint64_t)(uintptr_t)e,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            __atomic_store_n(&hdr[0], ++k, __ATOMIC_RELEASE);
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return st == ERR_PEER_CLOSED ? 0 : 2;
        signals_t seen;
        if (jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen) != OK)
            return 3;
    }
    return 0;
}

int svcstate_child(int argc, char **argv)
{
    const char *m = argv[1] + 9;   /* after "svcstate-" */
    if (!strcmp(m, "promoted"))
        return promoted(argc, argv);
    if (!strcmp(m, "chanread"))
        return chanread();
    /* "svcstate-spare" runs main only if libos never waited for a promotion. */
    printf("utest: svcstate mode \"%s\": ran main unpromoted\n", m);
    return 99;
}
