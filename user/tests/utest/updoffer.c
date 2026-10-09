/* utest: the offer call of `update` (abi/idl/update.idl) between its two
 * halves in <update.h>: update_make_offer (the fetcher's) and
 * update_answer_reply (init's), over a channel with a thread serving it as
 * init does, answering `later`.
 *
 *   update_offer_call   every field of the offer reaches the server (flags,
 *                       the three lengths, the manifest's bytes, two or
 *                       three VMOs) and every field of the answer comes back
 *                       (numbers and strings); an answer whose text is not
 *                       printable ASCII (a menu's reason, a manifest's
 *                       line) still goes, with '?' for those bytes, where
 *                       the generated reply would otherwise be an
 *                       ERR_INTERNAL; the client refuses a manifest past
 *                       1024 bytes and a fourth VMO before sending, and a
 *                       handle that is no VMO is refused by the server;
 *                       the VMOs of a refused offer are gone from the
 *                       caller's table all the same. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/update.h>
#include <os.h>
#include <update.h>
#include "utest.h"

#define SERVE_STACK (32u << 10)
#define OFFER_WAIT  (10 * NS_PER_S)

/* What the mock saw of the last offer, and what it answers. */
struct mock {
    uint32_t flags, manifest_len, nfiles, offers;
    uint64_t bytes[UPDATE_PARTS];
    uint8_t  manifest[UPDATE_MANIFEST_MAX];
    struct update_answer answer;
};

static status_t mock_offer(void *ctx, struct idl_txn txn, uint32_t flags, uint64_t kernel_bytes,
                           uint64_t bootfs_bytes, uint64_t menu_bytes, const uint8_t *manifest,
                           uint32_t manifest_len, const handle_t *files, uint32_t files_len,
                           int32_t *o0, uint32_t *o1, uint32_t *o2, uint32_t *o3, char *o4,
                           uint32_t *o5, char *o6, uint32_t *o7, uint32_t *o8, uint32_t *o9,
                           uint32_t *o10, char *o11, uint32_t *o12, char *o13, uint32_t *o14,
                           char *o15, uint32_t *o16, uint32_t *o17, int32_t *o18, char *o19,
                           uint32_t *o20, uint32_t *o21)
{
    struct mock *m = ctx;
    (void)o0, (void)o1, (void)o2, (void)o3, (void)o4, (void)o5, (void)o6, (void)o7, (void)o8;
    (void)o9, (void)o10, (void)o11, (void)o12, (void)o13, (void)o14, (void)o15, (void)o16;
    (void)o17, (void)o18, (void)o19, (void)o20, (void)o21;
    m->offers++;
    m->flags = flags;
    m->bytes[UPDATE_KERNEL] = kernel_bytes;
    m->bytes[UPDATE_BOOTFS] = bootfs_bytes;
    m->bytes[UPDATE_MENU] = menu_bytes;
    m->manifest_len = manifest_len;
    memcpy(m->manifest, manifest, manifest_len);
    m->nfiles = files_len;
    for (uint32_t i = 0; i < files_len; i++)
        jam_handle_close(files[i]);   /* the handler owns them */
    (void)update_answer_reply(txn.ch, txn.txid, &m->answer);
    return IDL_LATER;
}

static const struct update_ops mock_ops = { .offer = mock_offer };

struct serving {
    handle_t ch;
    struct mock *m;
};

static void serve(void *arg)
{
    struct serving *s = arg;
    (void)update_serve(s->ch, &mock_ops, s->m);
}

static bool vmos(handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &hs[i]), OK);
    return true;
}

static bool gone(const handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        CHECK_ST(jam_handle_close(hs[i]), ERR_BAD_HANDLE);
    return true;
}

/* The answer the mock gives: every field distinct. */
static void fill_answer(struct update_answer *a)
{
    *a = (struct update_answer){ .status = ERR_ACCESS_DENIED, .why = UPDATE_BAD_SIGNATURE,
        .file = UPDATE_MENU, .check_ms = 1234, .write_step = UPDATE_WRITE_NEW,
        .stick = UPDATE_STICK_OLD, .write_ms = 5678, .menu = UPDATE_MENU_REFUSED,
        .menu_status = ERR_IO, .already = 1 };
    snprintf(a->version, sizeof(a->version), "0.1.0-test");
    snprintf(a->git, sizeof(a->git), "abcdef0-dirty");
    snprintf(a->net, sizeof(a->net), "vlan21");
    snprintf(a->net_running, sizeof(a->net_running), "untagged");
    snprintf(a->needs, sizeof(a->needs), "!future-must");
    snprintf(a->menu_why, sizeof(a->menu_why), "line 3: no such entry");
}

static bool same_answer(const struct update_answer *got, const struct update_answer *want)
{
    CHECK_EQ(got->status, want->status);
    CHECK_EQ(got->why, want->why);
    CHECK_EQ(got->file, want->file);
    CHECK_EQ(got->check_ms, want->check_ms);
    CHECK_EQ(got->write_step, want->write_step);
    CHECK_EQ(got->stick, want->stick);
    CHECK_EQ(got->write_ms, want->write_ms);
    CHECK_EQ(got->menu, want->menu);
    CHECK_EQ(got->menu_status, want->menu_status);
    CHECK_EQ(got->already, want->already);
    CHECK(!strcmp(got->version, want->version) && !strcmp(got->git, want->git));
    CHECK(!strcmp(got->net, want->net) && !strcmp(got->net_running, want->net_running));
    CHECK(!strcmp(got->needs, want->needs) && !strcmp(got->menu_why, want->menu_why));
    return true;
}

bool t_update_offer_call(void)
{
    static struct mock m;
    static uint8_t manifest[UPDATE_MANIFEST_MAX + 1];
    handle_t cli, srv, th, hs[4];
    static uint8_t stack[SERVE_STACK];
    struct serving s = { 0 };
    struct update_answer a;
    uint64_t bytes[UPDATE_PARTS] = { 10, 20, 30 };
    for (unsigned i = 0; i < sizeof(manifest); i++)
        manifest[i] = (uint8_t)(i * 7 + 1);
    memset(&m, 0, sizeof(m));
    fill_answer(&m.answer);
    CHECK_ST(jam_channel_create(&cli, &srv), OK);
    s.ch = srv;
    s.m = &m;
    CHECK_ST(thread_spawn("updoffer", serve, &s, stack, sizeof(stack), &th), OK);

    /* all three files: what the server saw, what came back */
    CHECK(vmos(hs, 3));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, UPDATE_OFFER_WRITE | UPDATE_OFFER_FORCE,
                               bytes, manifest, 1000, hs, 3, &a), OK);
    CHECK_EQ(m.offers, 1);
    CHECK_EQ(m.flags, UPDATE_OFFER_WRITE | UPDATE_OFFER_FORCE);
    CHECK(!memcmp(m.bytes, bytes, sizeof(bytes)));
    CHECK_EQ(m.manifest_len, 1000);
    CHECK(!memcmp(m.manifest, manifest, 1000));
    CHECK_EQ(m.nfiles, 3);
    CHECK(same_answer(&a, &m.answer));

    /* two files, a manifest of exactly the most */
    CHECK(vmos(hs, 2));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest, UPDATE_MANIFEST_MAX,
                               hs, 2, &a), OK);
    CHECK_EQ(m.offers, 2);
    CHECK_EQ(m.nfiles, 2);
    CHECK_EQ(m.manifest_len, UPDATE_MANIFEST_MAX);

    /* an answer whose text is hostile: control characters, bytes that are no
     * UTF-8, strings filling their arrays to the last byte */
    fill_answer(&m.answer);
    memset(m.answer.menu_why, 'w', sizeof(m.answer.menu_why) - 1);
    m.answer.menu_why[3] = '\n';
    m.answer.menu_why[4] = (char)0xff;
    m.answer.menu_why[5] = (char)0xc0;
    m.answer.menu_why[6] = '\x1b';
    memset(m.answer.version, 'v', sizeof(m.answer.version) - 1);
    memset(m.answer.needs, '!', sizeof(m.answer.needs) - 1);
    m.answer.needs[2] = (char)0x80;
    CHECK(vmos(hs, 2));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest, 10, hs, 2, &a), OK);
    CHECK_EQ(strlen(a.menu_why), sizeof(a.menu_why) - 1);
    CHECK(a.menu_why[0] == 'w' && a.menu_why[3] == '?' && a.menu_why[4] == '?' &&
          a.menu_why[5] == '?' && a.menu_why[6] == '?' && a.menu_why[7] == 'w');
    CHECK_EQ(strlen(a.version), sizeof(a.version) - 1);
    CHECK(a.needs[2] == '?' && a.needs[1] == '!');
    CHECK_EQ(a.why, UPDATE_BAD_SIGNATURE);   /* the numbers came with them */

    /* refused by the client before it sends: a manifest past the most, a fourth VMO */
    uint32_t before = m.offers;
    CHECK(vmos(hs, 4));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest,
                               UPDATE_MANIFEST_MAX + 1, hs, 2, &a), ERR_INVALID_ARGS);
    CHECK(gone(hs, 2));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest, 10, hs + 2, 2, &a),
             OK);
    CHECK_EQ(m.offers, before + 1);
    CHECK(vmos(hs, 4));
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest, 10, hs, 4, &a),
             ERR_INVALID_ARGS);
    CHECK(gone(hs, 4));
    CHECK_EQ(m.offers, before + 1);
    CHECK_EQ(a.status, 0);   /* zeroed on a failure */

    /* a handle that is no VMO: refused by the server's type check, closed */
    CHECK_ST(jam_channel_create(&hs[0], &hs[1]), OK);
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &hs[2]), OK);
    handle_t files[2] = { hs[2], hs[0] };
    CHECK_ST(update_make_offer(cli, now() + OFFER_WAIT, 0, bytes, manifest, 10, files, 2, &a),
             ERR_WRONG_TYPE);
    CHECK_EQ(m.offers, before + 1);
    CHECK(gone(files, 2));
    jam_handle_close(hs[1]);

    jam_handle_close(cli);   /* the server returns */
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + OFFER_WAIT, NULL), OK);
    jam_handle_close(th);
    jam_handle_close(srv);
    return true;
}
